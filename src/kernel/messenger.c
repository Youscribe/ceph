#include <linux/crc32c.h>
#include <linux/kthread.h>
#include <linux/socket.h>
#include <linux/net.h>
#include <linux/string.h>
#include <linux/highmem.h>
#include <linux/ctype.h>
#include <net/tcp.h>

#include "ceph_debug.h"
#include "super.h"
#include "messenger.h"

/*
 * Ceph uses the messenger to exchange ceph_msg messages with other
 * hosts in the system.  The messenger provides ordered and reliable
 * delivery.  It tolerates TCP disconnects by reconnecting (with
 * exponential backoff) in the case of a fault (disconnection, bad
 * crc, protocol error).  Acks allow sent messages to be discarded by
 * the sender.
 *
 * The network topology is flat: there is no "client" or "server," and
 * any node can initiate a connection (i.e., send messages) to any
 * other node.  There is a fair bit of complexity to handle the
 * "connection race" case where two nodes are simultaneously
 * connecting to each other so that the end result is a single
 * session.
 *
 * The messenger can also send messages in "lossy" mode, where there
 * is no error recovery or connect retry... the message is just
 * dropped if something goes wrong.
 */


/* static tag bytes (protocol control messages) */
static char tag_msg = CEPH_MSGR_TAG_MSG;
static char tag_ack = CEPH_MSGR_TAG_ACK;
static char tag_keepalive = CEPH_MSGR_TAG_KEEPALIVE;


static void queue_con(struct ceph_connection *con);
static void con_work(struct work_struct *);
static void ceph_fault(struct ceph_connection *con);


/*
 * work queue for all reading and writing to/from the socket.
 */
struct workqueue_struct *ceph_msgr_wq;

int ceph_msgr_init(void)
{
	ceph_msgr_wq = create_workqueue("ceph-msgr");
	if (IS_ERR(ceph_msgr_wq)) {
		int ret = PTR_ERR(ceph_msgr_wq);
		pr_err("ceph_msgr_init failed to create workqueue: %d\n", ret);
		ceph_msgr_wq = NULL;
		return ret;
	}
	return 0;
}

void ceph_msgr_exit(void)
{
	destroy_workqueue(ceph_msgr_wq);
}

/*
 * socket callback functions
 */

/* data available on socket, or listen socket received a connect */
static void ceph_data_ready(struct sock *sk, int count_unused)
{
	struct ceph_connection *con =
		(struct ceph_connection *)sk->sk_user_data;
	if (sk->sk_state != TCP_CLOSE_WAIT) {
		dout("ceph_data_ready on %p state = %lu, queueing work\n",
		     con, con->state);
		queue_con(con);
	}
}

/* socket has buffer space for writing */
static void ceph_write_space(struct sock *sk)
{
	struct ceph_connection *con =
		(struct ceph_connection *)sk->sk_user_data;

	/* only queue to workqueue if there is data we want to write. */
	if (test_bit(WRITE_PENDING, &con->state)) {
		dout("ceph_write_space %p queueing write work\n", con);
		queue_con(con);
	} else {
		dout("ceph_write_space %p nothing to write\n", con);
	}

	/* since we have our own write_space, clear the SOCK_NOSPACE flag */
	clear_bit(SOCK_NOSPACE, &sk->sk_socket->flags);
}

/* socket's state has changed */
static void ceph_state_change(struct sock *sk)
{
	struct ceph_connection *con =
		(struct ceph_connection *)sk->sk_user_data;

	dout("ceph_state_change %p state = %lu sk_state = %u\n",
	     con, con->state, sk->sk_state);

	if (test_bit(CLOSED, &con->state))
		return;

	switch (sk->sk_state) {
	case TCP_CLOSE:
		dout("ceph_state_change TCP_CLOSE\n");
	case TCP_CLOSE_WAIT:
		dout("ceph_state_change TCP_CLOSE_WAIT\n");
		set_bit(SOCK_CLOSED, &con->state);
		if (test_bit(CONNECTING, &con->state))
			con->error_msg = "connection failed";
		else
			con->error_msg = "socket closed";
		queue_con(con);
		break;
	case TCP_ESTABLISHED:
		dout("ceph_state_change TCP_ESTABLISHED\n");
		queue_con(con);
		break;
	}
}

/*
 * set up socket callbacks
 */
static void set_sock_callbacks(struct socket *sock,
			       struct ceph_connection *con)
{
	struct sock *sk = sock->sk;
	sk->sk_user_data = (void *)con;
	sk->sk_data_ready = ceph_data_ready;
	sk->sk_write_space = ceph_write_space;
	sk->sk_state_change = ceph_state_change;
}


/*
 * socket helpers
 */

/*
 * initiate connection to a remote socket.
 */
static struct socket *ceph_tcp_connect(struct ceph_connection *con)
{
	struct sockaddr *paddr = (struct sockaddr *)&con->peer_addr.ipaddr;
	struct socket *sock;
	int ret;

	BUG_ON(con->sock);
	ret = sock_create_kern(AF_INET, SOCK_STREAM, IPPROTO_TCP, &sock);
	if (ret)
		return ERR_PTR(ret);
	con->sock = sock;
	sock->sk->sk_allocation = GFP_NOFS;

	set_sock_callbacks(sock, con);

	dout("connect %u.%u.%u.%u:%u\n",
	     IPQUADPORT(*(struct sockaddr_in *)paddr));

	ret = sock->ops->connect(sock, paddr,
				 sizeof(struct sockaddr_in), O_NONBLOCK);
	if (ret == -EINPROGRESS) {
		dout("connect %u.%u.%u.%u:%u EINPROGRESS sk_state = %u\n",
		     IPQUADPORT(*(struct sockaddr_in *)paddr),
		     sock->sk->sk_state);
		ret = 0;
	}
	if (ret < 0) {
		pr_err("ceph connect %u.%u.%u.%u:%u error %d\n",
			IPQUADPORT(*(struct sockaddr_in *)paddr), ret);
		sock_release(sock);
		con->sock = NULL;
		con->error_msg = "connect error";
	}

	if (ret < 0)
		return ERR_PTR(ret);
	return sock;
}

static int ceph_tcp_recvmsg(struct socket *sock, void *buf, size_t len)
{
	struct kvec iov = {buf, len};
	struct msghdr msg = { .msg_flags = MSG_DONTWAIT | MSG_NOSIGNAL };

	return kernel_recvmsg(sock, &msg, &iov, 1, len, msg.msg_flags);
}

/*
 * write something.  @more is true if caller will be sending more data
 * shortly.
 */
static int ceph_tcp_sendmsg(struct socket *sock, struct kvec *iov,
		     size_t kvlen, size_t len, int more)
{
	struct msghdr msg = { .msg_flags = MSG_DONTWAIT | MSG_NOSIGNAL };

	if (more)
		msg.msg_flags |= MSG_MORE;
	else
		msg.msg_flags |= MSG_EOR;  /* superfluous, but what the hell */

	return kernel_sendmsg(sock, &msg, iov, kvlen, len);
}


/*
 * Shutdown/close the socket for the given connection.
 */
static int con_close_socket(struct ceph_connection *con)
{
	int rc;

	dout("con_close_socket on %p sock %p\n", con, con->sock);
	if (!con->sock)
		return 0;
	rc = con->sock->ops->shutdown(con->sock, SHUT_RDWR);
	sock_release(con->sock);
	con->sock = NULL;
	return rc;
}

/*
 * Reset a connection.  Discard all incoming and outgoing messages
 * and clear *_seq state.
 */
static void reset_connection(struct ceph_connection *con)
{
	/* reset connection, out_queue, msg_ and connect_seq */
	/* discard existing out_queue and msg_seq */
	spin_lock(&con->out_queue_lock);
	ceph_msg_put_list(&con->out_queue);
	ceph_msg_put_list(&con->out_sent);

	con->connect_seq = 0;
	con->out_seq = 0;
	con->out_msg = NULL;
	con->in_seq = 0;
	spin_unlock(&con->out_queue_lock);
}

/*
 * mark a peer down.  drop any open connections.
 */
void ceph_con_close(struct ceph_connection *con)
{
	dout("close %p peer %u.%u.%u.%u:%u\n", con,
	     IPQUADPORT(con->peer_addr.ipaddr));
	set_bit(CLOSED, &con->state);  /* in case there's queued work */
	reset_connection(con);
	queue_con(con);
}

/*
 * clean up connection state
 */
void ceph_con_destroy(struct ceph_connection *con)
{
	dout("con_destroy %p destroying\n", con);
	reset_connection(con);
	set_bit(DEAD, &con->state);
	con_close_socket(con); /* silently ignore errors */
}

/*
 * Reopen a closed connection, with a new peer address.
 */
void ceph_con_reopen(struct ceph_connection *con, struct ceph_entity_addr *addr)
{
	dout("con_reopen %p %u.%u.%u.%u:%u\n", con, IPQUADPORT(addr->ipaddr));
	BUG_ON(!test_bit(CLOSED, &con->state));
	set_bit(REOPEN, &con->state);
	clear_bit(CLOSED, &con->state);
	memcpy(&con->peer_addr, addr, sizeof(*addr));
	queue_con(con);
}

/*
 * generic get/put
 */
struct ceph_connection *ceph_con_get(struct ceph_connection *con)
{
	dout("con_get %p nref = %d -> %d\n", con,
	     atomic_read(&con->nref), atomic_read(&con->nref) + 1);
	if (atomic_inc_not_zero(&con->nref))
		return con;
	return NULL;
}

void ceph_con_put(struct ceph_connection *con)
{
	dout("con_put %p nref = %d -> %d\n", con,
	     atomic_read(&con->nref), atomic_read(&con->nref) - 1);
	BUG_ON(atomic_read(&con->nref) == 0);
	if (atomic_dec_and_test(&con->nref)) {
		ceph_con_destroy(con);
		kfree(con);
	}
}

/*
 * initialize a new connection.
 *
 * NOTE: assumes struct is initially zeroed!
 */
void ceph_con_init(struct ceph_messenger *msgr, struct ceph_connection *con,
		   struct ceph_entity_addr *addr)
{
	dout("con_init %p %u.%u.%u.%u:%u\n", con, IPQUADPORT(addr->ipaddr));
	atomic_set(&con->nref, 1);
	con->msgr = msgr;
	spin_lock_init(&con->out_queue_lock);
	INIT_LIST_HEAD(&con->out_queue);
	INIT_LIST_HEAD(&con->out_sent);
	INIT_DELAYED_WORK(&con->work, con_work);
	con->peer_addr = *addr;

	con->private = NULL;
}


/*
 * We maintain a global counter to order connection attempts.  Get
 * a unique seq greater than @gt.
 */
static u32 get_global_seq(struct ceph_messenger *msgr, u32 gt)
{
	u32 ret;

	spin_lock(&msgr->global_seq_lock);
	if (msgr->global_seq < gt)
		msgr->global_seq = gt;
	ret = ++msgr->global_seq;
	spin_unlock(&msgr->global_seq_lock);
	return ret;
}


/*
 * Prepare footer for currently outgoing message, and finish things
 * off.  Assumes out_kvec* are already valid.. we just add on to the end.
 */
static void prepare_write_message_footer(struct ceph_connection *con, int v)
{
	struct ceph_msg *m = con->out_msg;

	dout("prepare_write_message_footer %p\n", con);
	con->out_kvec[v].iov_base = &m->footer;
	con->out_kvec[v].iov_len = sizeof(m->footer);
	con->out_kvec_bytes += sizeof(m->footer);
	con->out_kvec_left++;
	con->out_more = m->more_to_follow;
	con->out_msg = NULL;   /* we're done with this one */
}

/*
 * Prepare headers for the next outgoing message.
 */
static void prepare_write_message(struct ceph_connection *con)
{
	struct ceph_msg *m;
	int v = 0;

	con->out_kvec_bytes = 0;

	/* Sneak an ack in there first?  If we can get it into the same
	 * TCP packet that's a good thing. */
	if (con->in_seq > con->in_seq_acked) {
		con->in_seq_acked = con->in_seq;
		con->out_kvec[v].iov_base = &tag_ack;
		con->out_kvec[v++].iov_len = 1;
		con->out_temp_ack = cpu_to_le32(con->in_seq_acked);
		con->out_kvec[v].iov_base = &con->out_temp_ack;
		con->out_kvec[v++].iov_len = 4;
		con->out_kvec_bytes = 1 + 4;
	}

	/* move message to sending/sent list */
	m = list_first_entry(&con->out_queue,
		       struct ceph_msg, list_head);
	list_move_tail(&m->list_head, &con->out_sent);
	con->out_msg = m;   /* we don't bother taking a reference here. */

	dout("prepare_write_message %p seq %lld type %d len %d+%d+%d %d pgs\n",
	     m, le64_to_cpu(m->hdr.seq), le16_to_cpu(m->hdr.type),
	     le32_to_cpu(m->hdr.front_len), le32_to_cpu(m->hdr.middle_len),
	     le32_to_cpu(m->hdr.data_len),
	     m->nr_pages);
	BUG_ON(le32_to_cpu(m->hdr.front_len) != m->front.iov_len);

	/* tag + hdr + front + middle */
	con->out_kvec[v].iov_base = &tag_msg;
	con->out_kvec[v++].iov_len = 1;
	con->out_kvec[v].iov_base = &m->hdr;
	con->out_kvec[v++].iov_len = sizeof(m->hdr);
	con->out_kvec[v++] = m->front;
	if (m->middle)
		con->out_kvec[v++] = m->middle->vec;
	con->out_kvec_left = v;
	con->out_kvec_bytes += 1 + sizeof(m->hdr) + m->front.iov_len +
		(m->middle ? m->middle->vec.iov_len : 0);
	con->out_kvec_cur = con->out_kvec;

	/* fill in crc (except data pages), footer */
	con->out_msg->hdr.crc =
		cpu_to_le32(crc32c(0, (void *)&m->hdr,
				      sizeof(m->hdr) - sizeof(m->hdr.crc)));
	con->out_msg->footer.flags = 0;
	con->out_msg->footer.front_crc =
		cpu_to_le32(crc32c(0, m->front.iov_base, m->front.iov_len));
	if (m->middle)
		con->out_msg->footer.middle_crc =
			cpu_to_le32(crc32c(0, m->middle->vec.iov_base,
					   m->middle->vec.iov_len));
	else
		con->out_msg->footer.middle_crc = 0;
	con->out_msg->footer.data_crc = 0;
	dout("prepare_write_message front_crc %u data_crc %u\n",
	     le32_to_cpu(con->out_msg->footer.front_crc),
	     le32_to_cpu(con->out_msg->footer.middle_crc));

	/* is there a data payload? */
	if (le32_to_cpu(m->hdr.data_len) > 0) {
		/* initialize page iterator */
		con->out_msg_pos.page = 0;
		con->out_msg_pos.page_pos =
			le16_to_cpu(m->hdr.data_off) & ~PAGE_MASK;
		con->out_msg_pos.data_pos = 0;
		con->out_msg_pos.did_page_crc = 0;
		con->out_more = 1;  /* data + footer will follow */
	} else {
		/* no, queue up footer too and be done */
		prepare_write_message_footer(con, v);
	}

	set_bit(WRITE_PENDING, &con->state);
}

/*
 * Prepare an ack.
 */
static void prepare_write_ack(struct ceph_connection *con)
{
	dout("prepare_write_ack %p %u -> %u\n", con,
	     con->in_seq_acked, con->in_seq);
	con->in_seq_acked = con->in_seq;

	con->out_kvec[0].iov_base = &tag_ack;
	con->out_kvec[0].iov_len = 1;
	con->out_temp_ack = cpu_to_le32(con->in_seq_acked);
	con->out_kvec[1].iov_base = &con->out_temp_ack;
	con->out_kvec[1].iov_len = 4;
	con->out_kvec_left = 2;
	con->out_kvec_bytes = 1 + 4;
	con->out_kvec_cur = con->out_kvec;
	con->out_more = 1;  /* more will follow.. eventually.. */
	set_bit(WRITE_PENDING, &con->state);
}

/*
 * Prepare to write keepalive byte.
 */
static void prepare_write_keepalive(struct ceph_connection *con)
{
	dout("prepare_write_keepalive %p\n", con);
	con->out_kvec[0].iov_base = &tag_keepalive;
	con->out_kvec[0].iov_len = 1;
	con->out_kvec_left = 1;
	con->out_kvec_bytes = 1;
	con->out_kvec_cur = con->out_kvec;
	set_bit(WRITE_PENDING, &con->state);
}

/*
 * Connection negotiation.
 */

/*
 * We connected to a peer and are saying hello.
 */
static void prepare_write_connect(struct ceph_messenger *msgr,
				  struct ceph_connection *con)
{
	int len = strlen(CEPH_BANNER);

	dout("prepare_write_connect %p\n", con);
	con->out_connect.host_type = cpu_to_le32(CEPH_ENTITY_TYPE_CLIENT);
	con->out_connect.connect_seq = cpu_to_le32(con->connect_seq);
	con->out_connect.global_seq =
		cpu_to_le32(get_global_seq(con->msgr, 0));
	con->out_connect.flags = 0;
	if (test_bit(LOSSYTX, &con->state))
		con->out_connect.flags = CEPH_MSG_CONNECT_LOSSY;

	con->out_kvec[0].iov_base = CEPH_BANNER;
	con->out_kvec[0].iov_len = len;
	con->out_kvec[1].iov_base = &msgr->inst.addr;
	con->out_kvec[1].iov_len = sizeof(msgr->inst.addr);
	con->out_kvec[2].iov_base = &con->out_connect;
	con->out_kvec[2].iov_len = sizeof(con->out_connect);
	con->out_kvec_left = 3;
	con->out_kvec_bytes = len + sizeof(msgr->inst.addr) +
		sizeof(con->out_connect);
	con->out_kvec_cur = con->out_kvec;
	con->out_more = 0;
	set_bit(WRITE_PENDING, &con->state);
}

static void prepare_write_connect_retry(struct ceph_messenger *msgr,
					struct ceph_connection *con)
{
	dout("prepare_write_connect_retry %p\n", con);
	con->out_connect.connect_seq = cpu_to_le32(con->connect_seq);
	con->out_connect.global_seq =
		cpu_to_le32(get_global_seq(con->msgr, 0));

	con->out_kvec[0].iov_base = &con->out_connect;
	con->out_kvec[0].iov_len = sizeof(con->out_connect);
	con->out_kvec_left = 1;
	con->out_kvec_bytes = sizeof(con->out_connect);
	con->out_kvec_cur = con->out_kvec;
	con->out_more = 0;
	set_bit(WRITE_PENDING, &con->state);
}


/*
 * write as much of pending kvecs to the socket as we can.
 *  1 -> done
 *  0 -> socket full, but more to do
 * <0 -> error
 */
static int write_partial_kvec(struct ceph_connection *con)
{
	int ret;

	dout("write_partial_kvec %p %d left\n", con, con->out_kvec_bytes);
	while (con->out_kvec_bytes > 0) {
		ret = ceph_tcp_sendmsg(con->sock, con->out_kvec_cur,
				       con->out_kvec_left, con->out_kvec_bytes,
				       con->out_more);
		if (ret <= 0)
			goto out;
		con->out_kvec_bytes -= ret;
		if (con->out_kvec_bytes == 0)
			break;            /* done */
		while (ret > 0) {
			if (ret >= con->out_kvec_cur->iov_len) {
				ret -= con->out_kvec_cur->iov_len;
				con->out_kvec_cur++;
				con->out_kvec_left--;
			} else {
				con->out_kvec_cur->iov_len -= ret;
				con->out_kvec_cur->iov_base += ret;
				ret = 0;
				break;
			}
		}
	}
	con->out_kvec_left = 0;
	ret = 1;
out:
	dout("write_partial_kvec %p %d left in %d kvecs ret = %d\n", con,
	     con->out_kvec_bytes, con->out_kvec_left, ret);
	return ret;  /* done! */
}

/*
 * Write as much message data payload as we can.  If we finish, queue
 * up the footer.
 *  1 -> done, footer is now queued in out_kvec[].
 *  0 -> socket full, but more to do
 * <0 -> error
 */
static int write_partial_msg_pages(struct ceph_connection *con)
{
	struct ceph_client *client = con->msgr->parent;
	struct ceph_msg *msg = con->out_msg;
	unsigned data_len = le32_to_cpu(msg->hdr.data_len);
	size_t len;
	int crc = !ceph_test_opt(client, NOCRC);
	int ret;

	dout("write_partial_msg_pages %p msg %p page %d/%d offset %d\n",
	     con, con->out_msg, con->out_msg_pos.page, con->out_msg->nr_pages,
	     con->out_msg_pos.page_pos);

	while (con->out_msg_pos.page < con->out_msg->nr_pages) {
		struct page *page = NULL;
		void *kaddr = NULL;

		/*
		 * if we are calculating the data crc (the default), we need
		 * to map the page.  if our pages[] has been revoked, use the
		 * zero page.
		 */
		mutex_lock(&msg->page_mutex);
		if (msg->pages) {
			page = msg->pages[con->out_msg_pos.page];
			if (crc)
				kaddr = kmap(page);
		} else {
			page = con->msgr->zero_page;
			if (crc)
				kaddr = page_address(con->msgr->zero_page);
		}
		len = min((int)(PAGE_SIZE - con->out_msg_pos.page_pos),
			  (int)(data_len - con->out_msg_pos.data_pos));
		if (crc && !con->out_msg_pos.did_page_crc) {
			void *base = kaddr + con->out_msg_pos.page_pos;
			u32 tmpcrc = le32_to_cpu(con->out_msg->footer.data_crc);

			BUG_ON(kaddr == NULL);
			con->out_msg->footer.data_crc =
				cpu_to_le32(crc32c(tmpcrc, base, len));
			con->out_msg_pos.did_page_crc = 1;
		}

		ret = kernel_sendpage(con->sock, page,
				      con->out_msg_pos.page_pos, len,
				      MSG_DONTWAIT | MSG_NOSIGNAL |
				      MSG_MORE);

		if (crc && msg->pages)
			kunmap(page);

		mutex_unlock(&msg->page_mutex);
		if (ret <= 0)
			goto out;

		con->out_msg_pos.data_pos += ret;
		con->out_msg_pos.page_pos += ret;
		if (ret == len) {
			con->out_msg_pos.page_pos = 0;
			con->out_msg_pos.page++;
			con->out_msg_pos.did_page_crc = 0;
		}
	}

	dout("write_partial_msg_pages %p msg %p done\n", con, msg);

	/* prepare and queue up footer, too */
	if (!crc)
		con->out_msg->footer.flags |=
			cpu_to_le32(CEPH_MSG_FOOTER_NOCRC);
	con->out_kvec_bytes = 0;
	con->out_kvec_left = 0;
	con->out_kvec_cur = con->out_kvec;
	prepare_write_message_footer(con, 0);
	ret = 1;
out:
	return ret;
}



/*
 * Prepare to read connection handshake, or an ack.
 */
static void prepare_read_connect(struct ceph_connection *con)
{
	dout("prepare_read_connect %p\n", con);
	con->in_base_pos = 0;
}

static void prepare_read_ack(struct ceph_connection *con)
{
	dout("prepare_read_ack %p\n", con);
	con->in_base_pos = 0;
}

static void prepare_read_tag(struct ceph_connection *con)
{
	dout("prepare_read_tag %p\n", con);
	con->in_base_pos = 0;
	con->in_tag = CEPH_MSGR_TAG_READY;
}

/*
 * Prepare to read a message.
 */
static int prepare_read_message(struct ceph_connection *con)
{
	dout("prepare_read_message %p\n", con);
	BUG_ON(con->in_msg != NULL);
	con->in_base_pos = 0;
	con->in_front_crc = con->in_middle_crc = con->in_data_crc = 0;
	return 0;
}


static int read_partial(struct ceph_connection *con,
			int *to, int size, void *object)
{
	*to += size;
	while (con->in_base_pos < *to) {
		int left = *to - con->in_base_pos;
		int have = size - left;
		int ret = ceph_tcp_recvmsg(con->sock, object + have, left);
		if (ret <= 0)
			return ret;
		con->in_base_pos += ret;
	}
	return 1;
}


/*
 * Read all or part of the connect-side handshake on a new connection
 */
static int read_partial_connect(struct ceph_connection *con)
{
	int ret, to = 0;

	dout("read_partial_connect %p at %d\n", con, con->in_base_pos);

	/* peer's banner */
	ret = read_partial(con, &to, strlen(CEPH_BANNER), con->in_banner);
	if (ret <= 0)
		goto out;
	ret = read_partial(con, &to, sizeof(con->actual_peer_addr),
			   &con->actual_peer_addr);
	if (ret <= 0)
		goto out;
	ret = read_partial(con, &to, sizeof(con->in_reply), &con->in_reply);
	if (ret <= 0)
		goto out;

	dout("read_partial_connect %p connect_seq = %u, global_seq = %u\n",
	     con, le32_to_cpu(con->in_reply.connect_seq),
	     le32_to_cpu(con->in_reply.global_seq));
out:
	return ret;
}

/*
 * Verify the hello banner looks okay.
 */
static int verify_hello(struct ceph_connection *con)
{
	if (memcmp(con->in_banner, CEPH_BANNER, strlen(CEPH_BANNER))) {
		pr_err("ceph connect to/from %u.%u.%u.%u:%u has bad banner\n",
		       IPQUADPORT(con->peer_addr.ipaddr));
		con->error_msg = "protocol error, bad banner";
		return -1;
	}
	return 0;
}

static int process_connect(struct ceph_connection *con)
{
	dout("process_connect on %p tag %d\n", con, (int)con->in_tag);

	if (verify_hello(con) < 0)
		return -1;

	/*
	 * Make sure the other end is who we wanted.  note that the other
	 * end may not yet know their ip address, so if it's 0.0.0.0, give
	 * them the benefit of the doubt.
	 */
	if (!ceph_entity_addr_is_local(&con->peer_addr,
				       &con->actual_peer_addr) &&
	    !(con->actual_peer_addr.ipaddr.sin_addr.s_addr == 0 &&
	      con->actual_peer_addr.ipaddr.sin_port ==
	      con->peer_addr.ipaddr.sin_port &&
	      con->actual_peer_addr.nonce == con->peer_addr.nonce)) {
		pr_err("ceph wrong peer, want %u.%u.%u.%u:%u/%d, "
		       "got %u.%u.%u.%u:%u/%d, wtf\n",
		       IPQUADPORT(con->peer_addr.ipaddr),
		       con->peer_addr.nonce,
		       IPQUADPORT(con->actual_peer_addr.ipaddr),
		       con->actual_peer_addr.nonce);
		con->error_msg = "protocol error, wrong peer";
		return -1;
	}

	switch (con->in_reply.tag) {
	case CEPH_MSGR_TAG_RESETSESSION:
		/*
		 * If we connected with a large connect_seq but the peer
		 * has no record of a session with us (no connection, or
		 * connect_seq == 0), they will send RESETSESION to indicate
		 * that they must have reset their session, and may have
		 * dropped messages.
		 */
		dout("process_connect got RESET peer seq %u\n",
		     le32_to_cpu(con->in_connect.connect_seq));
		pr_err("ceph %s%d %u.%u.%u.%u:%u connection reset\n",
		       ENTITY_NAME(con->peer_name),
		       IPQUADPORT(con->peer_addr.ipaddr));
		reset_connection(con);
		prepare_write_connect_retry(con->msgr, con);
		prepare_read_connect(con);

		/* Tell ceph about it. */
		pr_info("reset on %s%d\n", ENTITY_NAME(con->peer_name));
		con->ops->peer_reset(con);
		break;

	case CEPH_MSGR_TAG_RETRY_SESSION:
		/*
		 * If we sent a smaller connect_seq than the peer has, try
		 * again with a larger value.
		 */
		dout("process_connect got RETRY my seq = %u, peer_seq = %u\n",
		     le32_to_cpu(con->out_connect.connect_seq),
		     le32_to_cpu(con->in_connect.connect_seq));
		con->connect_seq = le32_to_cpu(con->in_connect.connect_seq);
		prepare_write_connect_retry(con->msgr, con);
		prepare_read_connect(con);
		break;

	case CEPH_MSGR_TAG_RETRY_GLOBAL:
		/*
		 * If we sent a smaller global_seq than the peer has, try
		 * again with a larger value.
		 */
		dout("process_connect got RETRY_GLOBAL my %u, peer_gseq = %u\n",
		     con->peer_global_seq,
		     le32_to_cpu(con->in_connect.global_seq));
		get_global_seq(con->msgr,
			       le32_to_cpu(con->in_connect.global_seq));
		prepare_write_connect_retry(con->msgr, con);
		prepare_read_connect(con);
		break;

	case CEPH_MSGR_TAG_WAIT:
		/*
		 * If there is a connection race (we are opening connections to
		 * each other), one of us may just have to WAIT.  We will keep
		 * our queued messages, in expectation of being replaced by an
		 * incoming connection.
		 */
		dout("process_connect peer connecting WAIT\n");
		set_bit(WAIT, &con->state);
		con_close_socket(con);
		break;

	case CEPH_MSGR_TAG_READY:
		clear_bit(CONNECTING, &con->state);
		if (con->in_reply.flags & CEPH_MSG_CONNECT_LOSSY)
			set_bit(LOSSYRX, &con->state);
		con->peer_global_seq = le32_to_cpu(con->in_reply.global_seq);
		con->connect_seq++;
		dout("process_connect got READY gseq %d cseq %d (%d)\n",
		     con->peer_global_seq,
		     le32_to_cpu(con->in_reply.connect_seq),
		     con->connect_seq);
		WARN_ON(con->connect_seq !=
			le32_to_cpu(con->in_reply.connect_seq));

		con->delay = 0;  /* reset backoff memory */
		prepare_read_tag(con);
		break;

	default:
		pr_err("ceph connect protocol error, will retry\n");
		con->error_msg = "protocol error, garbage tag during connect";
		return -1;
	}
	return 0;
}


/*
 * read (part of) an ack
 */
static int read_partial_ack(struct ceph_connection *con)
{
	int to = 0;

	return read_partial(con, &to, sizeof(con->in_temp_ack),
			    &con->in_temp_ack);
}


/*
 * We can finally discard anything that's been acked.
 */
static void process_ack(struct ceph_connection *con)
{
	struct ceph_msg *m;
	u32 ack = le32_to_cpu(con->in_temp_ack);
	u64 seq;

	spin_lock(&con->out_queue_lock);
	while (!list_empty(&con->out_sent)) {
		m = list_first_entry(&con->out_sent, struct ceph_msg,
				     list_head);
		seq = le64_to_cpu(m->hdr.seq);
		if (seq > ack)
			break;
		dout("got ack for seq %llu type %d at %p\n", seq,
		     le16_to_cpu(m->hdr.type), m);
		ceph_msg_remove(m);
	}
	spin_unlock(&con->out_queue_lock);
	prepare_read_tag(con);
}






/*
 * read (part of) a message.
 */
static int read_partial_message(struct ceph_connection *con)
{
	struct ceph_msg *m = con->in_msg;
	void *p;
	int ret;
	int to, want, left;
	unsigned front_len, middle_len, data_len, data_off;
	struct ceph_client *client = con->msgr->parent;
	int datacrc = !ceph_test_opt(client, NOCRC);

	dout("read_partial_message con %p msg %p\n", con, m);

	/* header */
	while (con->in_base_pos < sizeof(con->in_hdr)) {
		left = sizeof(con->in_hdr) - con->in_base_pos;
		ret = ceph_tcp_recvmsg(con->sock,
				       (char *)&con->in_hdr + con->in_base_pos,
				       left);
		if (ret <= 0)
			return ret;
		con->in_base_pos += ret;
		if (con->in_base_pos == sizeof(con->in_hdr)) {
			u32 crc = crc32c(0, (void *)&con->in_hdr,
				 sizeof(con->in_hdr) - sizeof(con->in_hdr.crc));
			if (crc != le32_to_cpu(con->in_hdr.crc)) {
				pr_err("ceph read_partial_message bad hdr "
				       " crc %u != expected %u\n",
				       crc, con->in_hdr.crc);
				return -EBADMSG;
			}
		}
	}

	front_len = le32_to_cpu(con->in_hdr.front_len);
	if (front_len > CEPH_MSG_MAX_FRONT_LEN)
		return -EIO;
	middle_len = le32_to_cpu(con->in_hdr.middle_len);
	if (middle_len > CEPH_MSG_MAX_DATA_LEN)
		return -EIO;
	data_len = le32_to_cpu(con->in_hdr.data_len);
	if (data_len > CEPH_MSG_MAX_DATA_LEN)
		return -EIO;

	/* allocate message? */
	if (!con->in_msg) {
		dout("got hdr type %d front %d data %d\n", con->in_hdr.type,
		     con->in_hdr.front_len, con->in_hdr.data_len);
		con->in_msg = con->ops->alloc_msg(con->msgr->parent,
						  &con->in_hdr);
		if (!con->in_msg) {
			/* skip this message */
			dout("alloc_msg returned NULL, skipping message\n");
			con->in_base_pos = -front_len - middle_len - data_len -
				sizeof(m->footer);
			con->in_tag = CEPH_MSGR_TAG_READY;
			return 0;
		}
		if (IS_ERR(con->in_msg)) {
			ret = PTR_ERR(con->in_msg);
			con->in_msg = NULL;
			con->error_msg = "out of memory for incoming message";
			return ret;
		}
		m = con->in_msg;
		m->front.iov_len = 0;    /* haven't read it yet */
		memcpy(&m->hdr, &con->in_hdr, sizeof(con->in_hdr));
	}

	/* front */
	while (m->front.iov_len < front_len) {
		BUG_ON(m->front.iov_base == NULL);
		left = front_len - m->front.iov_len;
		ret = ceph_tcp_recvmsg(con->sock, (char *)m->front.iov_base +
				       m->front.iov_len, left);
		if (ret <= 0)
			return ret;
		m->front.iov_len += ret;
		if (m->front.iov_len == front_len)
			con->in_front_crc = crc32c(0, m->front.iov_base,
						      m->front.iov_len);
	}

	/* middle */
	while (middle_len > 0 && (!m->middle ||
				  m->middle->vec.iov_len < middle_len)) {
		if (m->middle == NULL) {
			BUG_ON(!con->ops->alloc_middle);
			ret = con->ops->alloc_middle(con->msgr->parent, m);
			if (ret < 0) {
				dout("alloc_middle failed, skipping payload\n");
				con->in_base_pos = -middle_len - data_len
					- sizeof(m->footer);
				ceph_msg_put(con->in_msg);
				con->in_msg = NULL;
				con->in_tag = CEPH_MSGR_TAG_READY;
				return 0;
			}
			m->middle->vec.iov_len = 0;
		}
		left = middle_len - m->middle->vec.iov_len;
		ret = ceph_tcp_recvmsg(con->sock,
				       (char *)m->middle->vec.iov_base +
				       m->middle->vec.iov_len, left);
		if (ret <= 0)
			return ret;
		m->middle->vec.iov_len += ret;
		if (m->middle->vec.iov_len == middle_len)
			con->in_middle_crc = crc32c(0, m->middle->vec.iov_base,
						      m->middle->vec.iov_len);
	}

	/* (page) data */
	data_off = le16_to_cpu(m->hdr.data_off);
	if (data_len == 0)
		goto no_data;

	if (m->nr_pages == 0) {
		con->in_msg_pos.page = 0;
		con->in_msg_pos.page_pos = data_off & ~PAGE_MASK;
		con->in_msg_pos.data_pos = 0;
		/* find pages for data payload */
		want = calc_pages_for(data_off & ~PAGE_MASK, data_len);
		ret = 0;
		BUG_ON(!con->ops->prepare_pages);
		ret = con->ops->prepare_pages(con->msgr->parent, m, want);
		if (ret < 0) {
			dout("%p prepare_pages failed, skipping payload\n", m);
			con->in_base_pos = -data_len - sizeof(m->footer);
			ceph_msg_put(con->in_msg);
			con->in_msg = NULL;
			con->in_tag = CEPH_MSGR_TAG_READY;
			return 0;
		}
		BUG_ON(m->nr_pages < want);
	}
	while (con->in_msg_pos.data_pos < data_len) {
		left = min((int)(data_len - con->in_msg_pos.data_pos),
			   (int)(PAGE_SIZE - con->in_msg_pos.page_pos));
		mutex_lock(&m->page_mutex);
		if (!m->pages) {
			dout("%p pages revoked during msg read\n", m);
			mutex_unlock(&m->page_mutex);
			con->in_base_pos = con->in_msg_pos.data_pos
				- data_len - sizeof(m->footer);
			ceph_msg_put(m);
			con->in_msg = NULL;
			con->in_tag = CEPH_MSGR_TAG_READY;
			return 0;
		}
		p = kmap(m->pages[con->in_msg_pos.page]);
		ret = ceph_tcp_recvmsg(con->sock, p + con->in_msg_pos.page_pos,
				       left);
		if (ret > 0 && datacrc)
			con->in_data_crc =
				crc32c(con->in_data_crc,
					  p + con->in_msg_pos.page_pos, ret);
		kunmap(m->pages[con->in_msg_pos.page]);
		mutex_unlock(&m->page_mutex);
		if (ret <= 0)
			return ret;
		con->in_msg_pos.data_pos += ret;
		con->in_msg_pos.page_pos += ret;
		if (con->in_msg_pos.page_pos == PAGE_SIZE) {
			con->in_msg_pos.page_pos = 0;
			con->in_msg_pos.page++;
		}
	}

no_data:
	/* footer */
	to = sizeof(m->hdr) + sizeof(m->footer);
	while (con->in_base_pos < to) {
		left = to - con->in_base_pos;
		ret = ceph_tcp_recvmsg(con->sock, (char *)&m->footer +
				       (con->in_base_pos - sizeof(m->hdr)),
				       left);
		if (ret <= 0)
			return ret;
		con->in_base_pos += ret;
	}
	dout("read_partial_message got msg %p %d (%u) + %d (%u) + %d (%u)\n",
	     m, front_len, m->footer.front_crc, middle_len,
	     m->footer.middle_crc, data_len, m->footer.data_crc);

	/* crc ok? */
	if (con->in_front_crc != le32_to_cpu(m->footer.front_crc)) {
		pr_err("ceph read_partial_message %p front crc %u != exp. %u\n",
		       m, con->in_front_crc, m->footer.front_crc);
		return -EBADMSG;
	}
	if (con->in_middle_crc != le32_to_cpu(m->footer.middle_crc)) {
		pr_err("ceph read_partial_message %p middle crc %u != exp %u\n",
		       m, con->in_middle_crc, m->footer.middle_crc);
		return -EBADMSG;
	}
	if (datacrc &&
	    (le32_to_cpu(m->footer.flags) & CEPH_MSG_FOOTER_NOCRC) == 0 &&
	    con->in_data_crc != le32_to_cpu(m->footer.data_crc)) {
		pr_err("ceph read_partial_message %p data crc %u != exp. %u\n",
		       m,
		       con->in_data_crc, le32_to_cpu(m->footer.data_crc));
		return -EBADMSG;
	}

	return 1; /* done! */
}

/*
 * Process message.  This happens in the worker thread.  The callback should
 * be careful not to do anything that waits on other incoming messages or it
 * may deadlock.
 */
static void process_message(struct ceph_connection *con)
{
	struct ceph_msg *msg = con->in_msg;

	con->in_msg = NULL;

	/* if first message, set peer_name */
	if (con->peer_name.type == 0)
		con->peer_name = msg->hdr.src.name;

	spin_lock(&con->out_queue_lock);
	con->in_seq++;
	spin_unlock(&con->out_queue_lock);

	dout("===== %p %llu from %s%d %d=%s len %d+%d (%u %u %u) =====\n",
	     msg, le64_to_cpu(msg->hdr.seq),
	     ENTITY_NAME(msg->hdr.src.name),
	     le16_to_cpu(msg->hdr.type),
	     ceph_msg_type_name(le16_to_cpu(msg->hdr.type)),
	     le32_to_cpu(msg->hdr.front_len),
	     le32_to_cpu(msg->hdr.data_len),
	     con->in_front_crc, con->in_middle_crc, con->in_data_crc);
	con->ops->dispatch(con, msg);
	prepare_read_tag(con);
}








/*
 * Write something to the socket.  Called in a worker thread when the
 * socket appears to be writeable and we have something ready to send.
 */
static int try_write(struct ceph_connection *con)
{
	struct ceph_messenger *msgr = con->msgr;
	int ret = 1;

	dout("try_write start %p state %lu nref %d\n", con, con->state,
	     atomic_read(&con->nref));

more:
	dout("try_write out_kvec_bytes %d\n", con->out_kvec_bytes);

	/* open the socket first? */
	if (con->sock == NULL) {
		/*
		 * if we were STANDBY and are reconnecting _this_
		 * connection, bump connect_seq now.  Always bump
		 * global_seq.
		 */
		if (test_and_clear_bit(STANDBY, &con->state))
			con->connect_seq++;

		prepare_write_connect(msgr, con);
		prepare_read_connect(con);
		set_bit(CONNECTING, &con->state);

		con->in_tag = CEPH_MSGR_TAG_READY;
		dout("try_write initiating connect on %p new state %lu\n",
		     con, con->state);
		con->sock = ceph_tcp_connect(con);
		if (IS_ERR(con->sock)) {
			con->sock = NULL;
			con->error_msg = "connect error";
			ret = -1;
			goto out;
		}
	}

more_kvec:
	/* kvec data queued? */
	if (con->out_kvec_left) {
		ret = write_partial_kvec(con);
		if (ret <= 0)
			goto done;
		if (ret < 0) {
			dout("try_write write_partial_kvec err %d\n", ret);
			goto done;
		}
	}

	/* msg pages? */
	if (con->out_msg) {
		ret = write_partial_msg_pages(con);
		if (ret == 1)
			goto more_kvec;  /* we need to send the footer, too! */
		if (ret == 0)
			goto done;
		if (ret < 0) {
			dout("try_write write_partial_msg_pages err %d\n",
			     ret);
			goto done;
		}
	}

	if (!test_bit(CONNECTING, &con->state)) {
		/* is anything else pending? */
		spin_lock(&con->out_queue_lock);
		if (!list_empty(&con->out_queue)) {
			prepare_write_message(con);
			spin_unlock(&con->out_queue_lock);
			goto more;
		}
		if (con->in_seq > con->in_seq_acked) {
			prepare_write_ack(con);
			spin_unlock(&con->out_queue_lock);
			goto more;
		}
		spin_unlock(&con->out_queue_lock);
	}

	if (test_and_clear_bit(KEEPALIVE_PENDING, &con->state)) {
		prepare_write_keepalive(con);
		goto more_kvec;
	}

	/* Nothing to do! */
	clear_bit(WRITE_PENDING, &con->state);
	dout("try_write nothing else to write.\n");
done:
	ret = 0;
out:
	dout("try_write done on %p\n", con);
	return ret;
}



/*
 * Read what we can from the socket.
 */
static int try_read(struct ceph_connection *con)
{
	struct ceph_messenger *msgr;
	int ret = -1;

	if (!con->sock)
		return 0;

	if (test_bit(STANDBY, &con->state))
		return 0;

	dout("try_read start on %p\n", con);
	msgr = con->msgr;

more:
	dout("try_read tag %d in_base_pos %d\n", (int)con->in_tag,
	     con->in_base_pos);
	if (test_bit(CONNECTING, &con->state)) {
		dout("try_read connecting\n");
		ret = read_partial_connect(con);
		if (ret <= 0)
			goto done;
		if (process_connect(con) < 0 || test_bit(WAIT, &con->state)) {
			ret = -1;
			goto out;
		}
		goto more;
	}

	if (con->in_base_pos < 0) {
		/*
		 * skipping + discarding content.
		 *
		 * FIXME: there must be a better way to do this!
		 */
		static char buf[1024];
		int skip = min(1024, -con->in_base_pos);
		dout("skipping %d / %d bytes\n", skip, -con->in_base_pos);
		ret = ceph_tcp_recvmsg(con->sock, buf, skip);
		if (ret <= 0)
			goto done;
		con->in_base_pos += ret;
		if (con->in_base_pos)
			goto more;
	}
	if (con->in_tag == CEPH_MSGR_TAG_READY) {
		/*
		 * what's next?
		 */
		ret = ceph_tcp_recvmsg(con->sock, &con->in_tag, 1);
		if (ret <= 0)
			goto done;
		dout("try_read got tag %d\n", (int)con->in_tag);
		switch (con->in_tag) {
		case CEPH_MSGR_TAG_MSG:
			prepare_read_message(con);
			break;
		case CEPH_MSGR_TAG_ACK:
			prepare_read_ack(con);
			break;
		case CEPH_MSGR_TAG_CLOSE:
			set_bit(CLOSED, &con->state);   /* fixme */
			goto done;
		default:
			goto bad_tag;
		}
	}
	if (con->in_tag == CEPH_MSGR_TAG_MSG) {
		ret = read_partial_message(con);
		if (ret <= 0) {
			switch (ret) {
			case -EBADMSG:
				con->error_msg = "bad crc";
				ret = -EIO;
				goto out;
			case -EIO:
				con->error_msg = "io error";
				goto out;
			default:
				goto done;
			}
		}
		if (con->in_tag == CEPH_MSGR_TAG_READY)
			goto more;
		process_message(con);
		goto more;
	}
	if (con->in_tag == CEPH_MSGR_TAG_ACK) {
		ret = read_partial_ack(con);
		if (ret <= 0)
			goto done;
		process_ack(con);
		goto more;
	}

done:
	ret = 0;
out:
	dout("try_read done on %p\n", con);
	return ret;

bad_tag:
	pr_err("ceph try_read bad con->in_tag = %d\n", (int)con->in_tag);
	con->error_msg = "protocol error, garbage tag";
	ret = -1;
	goto out;
}


/*
 * Atomically queue work on a connection.  Bump @con reference to
 * avoid races with connection teardown.
 *
 * There is some trickery going on with QUEUED and BUSY because we
 * only want a _single_ thread operating on each connection at any
 * point in time, but we want to use all available CPUs.
 *
 * The worker thread only proceeds if it can atomically set BUSY.  It
 * clears QUEUED and does it's thing.  When it thinks it's done, it
 * clears BUSY, then rechecks QUEUED.. if it's set again, it loops
 * (tries again to set BUSY).
 *
 * To queue work, we first set QUEUED, _then_ if BUSY isn't set, we
 * try to queue work.  If that fails (work is already queued, or BUSY)
 * we give up (work also already being done or is queued) but leave QUEUED
 * set so that the worker thread will loop if necessary.
 */
static void queue_con(struct ceph_connection *con)
{
	if (test_bit(WAIT, &con->state) ||
	    test_bit(DEAD, &con->state)) {
		dout("queue_con %p ignoring: WAIT|DEAD\n",
		     con);
		return;
	}

	if (!con->ops->get(con)) {
		dout("queue_con %p ref count 0\n", con);
		return;
	}

	set_bit(QUEUED, &con->state);
	if (test_bit(BUSY, &con->state) ||
	    !queue_work(ceph_msgr_wq, &con->work.work)) {
		dout("queue_con %p - already BUSY or queued\n", con);
		con->ops->put(con);
	} else {
		dout("queue_con %p\n", con);
	}
}

/*
 * Do some work on a connection.  Drop a connection ref when we're done.
 */
static void con_work(struct work_struct *work)
{
	struct ceph_connection *con = container_of(work, struct ceph_connection,
						   work.work);
	int backoff = 0;

more:
	if (test_and_set_bit(BUSY, &con->state) != 0) {
		dout("con_work %p BUSY already set\n", con);
		goto out;
	}
	dout("con_work %p start, clearing QUEUED\n", con);
	clear_bit(QUEUED, &con->state);

	if (test_bit(CLOSED, &con->state)) { /* e.g. if we are replaced */
		dout("con_work CLOSED\n");
		con_close_socket(con);
		goto done;
	}
	if (test_and_clear_bit(REOPEN, &con->state)) {
		/* reopen w/ new peer */
		dout("con_work REOPEN\n");
		con_close_socket(con);
	}
	if (test_bit(WAIT, &con->state)) {   /* we are a zombie */
		dout("con_work WAIT\n");
		goto done;
	}

	if (test_and_clear_bit(SOCK_CLOSED, &con->state) ||
	    try_read(con) < 0 ||
	    try_write(con) < 0) {
		backoff = 1;
		ceph_fault(con);     /* error/fault path */
	}

done:
	clear_bit(BUSY, &con->state);
	dout("con->state=%lu\n", con->state);
	if (test_bit(QUEUED, &con->state)) {
		if (!backoff) {
			dout("con_work %p QUEUED reset, looping\n", con);
			goto more;
		}
		dout("con_work %p QUEUED reset, but just faulted\n", con);
		clear_bit(QUEUED, &con->state);
	}
	dout("con_work %p done\n", con);

out:
	con->ops->put(con);
}


/*
 * Generic error/fault handler.  A retry mechanism is used with
 * exponential backoff
 */
static void ceph_fault(struct ceph_connection *con)
{
	pr_err("ceph %s%d %u.%u.%u.%u:%u %s\n", ENTITY_NAME(con->peer_name),
	       IPQUADPORT(con->peer_addr.ipaddr), con->error_msg);
	dout("fault %p state %lu to peer %u.%u.%u.%u:%u\n",
	     con, con->state, IPQUADPORT(con->peer_addr.ipaddr));

	if (test_bit(LOSSYTX, &con->state)) {
		dout("fault on LOSSYTX channel\n");
		goto out;
	}

	clear_bit(BUSY, &con->state);  /* to avoid an improbable race */

	con_close_socket(con);
	con->in_msg = NULL;

	/* If there are no messages in the queue, place the connection
	 * in a STANDBY state (i.e., don't try to reconnect just yet). */
	spin_lock(&con->out_queue_lock);
	if (list_empty(&con->out_queue)) {
		dout("fault setting STANDBY\n");
		set_bit(STANDBY, &con->state);
		spin_unlock(&con->out_queue_lock);
		goto out;
	}

	/* Requeue anything that hasn't been acked, and retry after a
	 * delay. */
	list_splice_init(&con->out_sent, &con->out_queue);
	spin_unlock(&con->out_queue_lock);

	if (con->delay == 0)
		con->delay = BASE_DELAY_INTERVAL;
	else if (con->delay < MAX_DELAY_INTERVAL)
		con->delay *= 2;

	/* explicitly schedule work to try to reconnect again later. */
	dout("fault queueing %p delay %lu\n", con, con->delay);
	con->ops->get(con);
	if (queue_delayed_work(ceph_msgr_wq, &con->work,
			       round_jiffies_relative(con->delay)) == 0)
		con->ops->put(con);

out:
	if (con->ops->fault)
		con->ops->fault(con);
}



/*
 * create a new messenger instance, creates listening socket
 */
struct ceph_messenger *ceph_messenger_create(struct ceph_entity_addr *myaddr)
{
	struct ceph_messenger *msgr;

	msgr = kzalloc(sizeof(*msgr), GFP_KERNEL);
	if (msgr == NULL)
		return ERR_PTR(-ENOMEM);

	spin_lock_init(&msgr->global_seq_lock);

	/* the zero page is needed if a request is "canceled" while the message
	 * is being written over the socket */
	msgr->zero_page = alloc_page(GFP_KERNEL | __GFP_ZERO);
	if (!msgr->zero_page) {
		kfree(msgr);
		return ERR_PTR(-ENOMEM);
	}
	kmap(msgr->zero_page);

	/* pick listening address */
	if (myaddr) {
		msgr->inst.addr = *myaddr;
	} else {
		dout("create ip not specified, initially INADDR_ANY\n");
		msgr->inst.addr.ipaddr.sin_addr.s_addr = htonl(INADDR_ANY);
		msgr->inst.addr.ipaddr.sin_port = htons(0);  /* any port */
		msgr->inst.addr.nonce = get_random_int();
	}
	msgr->inst.addr.ipaddr.sin_family = AF_INET;

	if (myaddr)
		msgr->inst.addr.ipaddr.sin_addr = myaddr->ipaddr.sin_addr;

	dout("messenger_create %p\n", msgr);
	return msgr;
}

void ceph_messenger_destroy(struct ceph_messenger *msgr)
{
	dout("destroy %p\n", msgr);
	kunmap(msgr->zero_page);
	__free_page(msgr->zero_page);
	kfree(msgr);
	dout("destroyed messenger %p\n", msgr);
}

/*
 * A single ceph_msg can't be queued for send twice, unless it's
 * already been delivered (i.e. we have the only remaining reference),
 * because of the list_head indicating which queue it is on.
 *
 * So, we dup the message if there is more than once reference.  If it has
 * pages (a data payload), steal the pages away from the old message.
 */
struct ceph_msg *ceph_msg_maybe_dup(struct ceph_msg *old)
{
	struct ceph_msg *dup;

	if (atomic_read(&old->nref) == 1)
		return old;  /* we have only ref, all is well */

	dup = ceph_msg_new(le16_to_cpu(old->hdr.type),
			   le32_to_cpu(old->hdr.front_len),
			   le32_to_cpu(old->hdr.data_len),
			   le16_to_cpu(old->hdr.data_off),
			   old->pages);
	if (!dup)
		return ERR_PTR(-ENOMEM);
	memcpy(dup->front.iov_base, old->front.iov_base,
	       le32_to_cpu(old->hdr.front_len));

	/* revoke old message's pages */
	mutex_lock(&old->page_mutex);
	old->pages = NULL;
	old->footer.flags |= cpu_to_le32(CEPH_MSG_FOOTER_ABORTED);
	mutex_unlock(&old->page_mutex);

	ceph_msg_put(old);
	return dup;
}

/*
 * Queue up an outgoing message on the given connection.
 */
void ceph_con_send(struct ceph_connection *con, struct ceph_msg *msg)
{
	if (test_bit(CLOSED, &con->state)) {
		dout("con_send %p closed, dropping %p\n", con, msg);
		ceph_msg_put(msg);
		return;
	}

	/* set src+dst */
	msg->hdr.src = con->msgr->inst;
	msg->hdr.orig_src = con->msgr->inst;
	msg->hdr.dst_erank = con->peer_addr.erank;

	/* queue */
	spin_lock(&con->out_queue_lock);
	msg->hdr.seq = cpu_to_le64(++con->out_seq);
	dout("----- %p %u to %s%d %d=%s len %d+%d+%d -----\n", msg,
	     (unsigned)con->out_seq,
	     ENTITY_NAME(con->peer_name), le16_to_cpu(msg->hdr.type),
	     ceph_msg_type_name(le16_to_cpu(msg->hdr.type)),
	     le32_to_cpu(msg->hdr.front_len),
	     le32_to_cpu(msg->hdr.middle_len),
	     le32_to_cpu(msg->hdr.data_len));
	dout("ceph_con_send %p %s%d %p seq %llu pgs %d\n",
	     con, ENTITY_NAME(con->peer_name), msg,
	     le64_to_cpu(msg->hdr.seq), msg->nr_pages);
	list_add_tail(&msg->list_head, &con->out_queue);
	spin_unlock(&con->out_queue_lock);

	/* if there wasn't anything waiting to send before, queue
	 * new work */
	if (test_and_set_bit(WRITE_PENDING, &con->state) == 0)
		queue_con(con);
}

/*
 * Queue a keepalive byte to ensure the tcp connection is alive.
 */
void ceph_con_keepalive(struct ceph_connection *con)
{
	if (test_and_set_bit(KEEPALIVE_PENDING, &con->state) == 0 &&
	    test_and_set_bit(WRITE_PENDING, &con->state) == 0)
		queue_con(con);
}


/*
 * construct a new message with given type, size
 * the new msg has a ref count of 1.
 */
struct ceph_msg *ceph_msg_new(int type, int front_len,
			      int page_len, int page_off, struct page **pages)
{
	struct ceph_msg *m;

	m = kmalloc(sizeof(*m), GFP_NOFS);
	if (m == NULL)
		goto out;
	atomic_set(&m->nref, 1);
	mutex_init(&m->page_mutex);
	INIT_LIST_HEAD(&m->list_head);

	m->hdr.type = cpu_to_le16(type);
	m->hdr.front_len = cpu_to_le32(front_len);
	m->hdr.middle_len = 0;
	m->hdr.data_len = cpu_to_le32(page_len);
	m->hdr.data_off = cpu_to_le16(page_off);
	m->hdr.priority = cpu_to_le16(CEPH_MSG_PRIO_DEFAULT);
	m->hdr.mon_protocol = CEPH_MON_PROTOCOL;
	m->hdr.monc_protocol = CEPH_MONC_PROTOCOL;
	m->hdr.osd_protocol = CEPH_OSD_PROTOCOL;
	m->hdr.osdc_protocol = CEPH_OSDC_PROTOCOL;
	m->hdr.mds_protocol = CEPH_MDS_PROTOCOL;
	m->hdr.mdsc_protocol = CEPH_MDSC_PROTOCOL;
	m->footer.front_crc = 0;
	m->footer.middle_crc = 0;
	m->footer.data_crc = 0;
	m->front_max = front_len;
	m->front_is_vmalloc = false;
	m->more_to_follow = false;
	m->pool = NULL;

	/* front */
	if (front_len) {
		if (front_len > PAGE_CACHE_SIZE) {
			m->front.iov_base = __vmalloc(front_len, GFP_NOFS,
						      PAGE_KERNEL);
			m->front_is_vmalloc = true;
		} else {
			m->front.iov_base = kmalloc(front_len, GFP_NOFS);
		}
		if (m->front.iov_base == NULL) {
			pr_err("ceph_msg_new can't allocate %d bytes\n",
			     front_len);
			goto out2;
		}
	} else {
		m->front.iov_base = NULL;
	}
	m->front.iov_len = front_len;

	/* middle */
	m->middle = NULL;

	/* data */
	m->nr_pages = calc_pages_for(page_off, page_len);
	m->pages = pages;

	dout("ceph_msg_new %p page %d~%d -> %d\n", m, page_off, page_len,
	     m->nr_pages);
	return m;

out2:
	ceph_msg_put(m);
out:
	pr_err("ceph_msg_new can't create type %d len %d\n", type, front_len);
	return ERR_PTR(-ENOMEM);
}

/*
 * Generic message allocator, for incoming messages.
 */
struct ceph_msg *ceph_alloc_msg(struct ceph_connection *con,
				struct ceph_msg_header *hdr)
{
	int type = le32_to_cpu(hdr->type);
	int front_len = le32_to_cpu(hdr->front_len);
	struct ceph_msg *msg = ceph_msg_new(type, front_len, 0, 0, NULL);

	if (!msg) {
		pr_err("ceph: unable to allocate msg type %d len %d\n",
		       type, front_len);
		return ERR_PTR(-ENOMEM);
	}
	return msg;
}

/*
 * Allocate "middle" portion of a message, if it is needed and wasn't
 * allocated by alloc_msg.  This allows us to read a small fixed-size
 * per-type header in the front and then gracefully fail (i.e.,
 * propagate the error to the caller based on info in the front) when
 * the middle is too large.
 */
int ceph_alloc_middle(struct ceph_connection *con, struct ceph_msg *msg)
{
	int type = le32_to_cpu(msg->hdr.type);
	int middle_len = le32_to_cpu(msg->hdr.middle_len);

	dout("alloc_middle %p type %d %s middle_len %d\n", msg, type,
	     ceph_msg_type_name(type), middle_len);
	BUG_ON(!middle_len);
	BUG_ON(msg->middle);

	msg->middle = ceph_buffer_new_alloc(middle_len, GFP_NOFS);
	if (!msg->middle)
		return -ENOMEM;
	return 0;
}


/*
 * Free a generically kmalloc'd message.
 */
void ceph_msg_kfree(struct ceph_msg *m)
{
	dout("msg_kfree %p\n", m);
	if (m->front_is_vmalloc)
		vfree(m->front.iov_base);
	else
		kfree(m->front.iov_base);
	kfree(m);
}

/*
 * Drop a msg ref.  Destroy as needed.
 */
void ceph_msg_put(struct ceph_msg *m)
{
	dout("ceph_msg_put %p %d -> %d\n", m, atomic_read(&m->nref),
	     atomic_read(&m->nref)-1);
	if (atomic_read(&m->nref) <= 0) {
		pr_err("bad ceph_msg_put on %p %llu %d=%s %d+%d\n",
		       m, le64_to_cpu(m->hdr.seq),
		       le16_to_cpu(m->hdr.type),
		       ceph_msg_type_name(le16_to_cpu(m->hdr.type)),
		       le32_to_cpu(m->hdr.front_len),
		       le32_to_cpu(m->hdr.data_len));
		WARN_ON(1);
	}
	if (atomic_dec_and_test(&m->nref)) {
		dout("ceph_msg_put last one on %p\n", m);
		WARN_ON(!list_empty(&m->list_head));

		/* drop middle, data, if any */
		if (m->middle) {
			ceph_buffer_put(m->middle);
			m->middle = NULL;
		}
		m->nr_pages = 0;
		m->pages = NULL;

		if (m->pool)
			ceph_msgpool_put(m->pool, m);
		else
			ceph_msg_kfree(m);
	}
}
