#include <string.h>

#include <iostream>
#include <map>

#include "include/types.h"

#include "rgw_common.h"
#include "rgw_xml.h"

#define dout_subsys ceph_subsys_rgw

using namespace std;

XMLObjIter::
XMLObjIter()
{
}

XMLObjIter::
~XMLObjIter()
{
}

void XMLObjIter::
set(const XMLObjIter::map_iter_t &_cur, const XMLObjIter::map_iter_t &_end)
{
  cur = _cur;
  end = _end;
}

XMLObj *XMLObjIter::
get_next()
{
  XMLObj *obj = NULL;
  if (cur != end) {
    obj = cur->second;
    ++cur;
  }
  return obj;
};

ostream& operator<<(ostream& out, XMLObj& obj) {
   out << obj.obj_type << ": " << obj.data;
   return out;
}

XMLObj::
~XMLObj()
{
}

bool XMLObj::
xml_start(XMLObj *parent, const char *el, const char **attr)
{
  this->parent = parent;
  obj_type = el;
  for (int i = 0; attr[i]; i += 2) {
    attr_map[attr[i]] = string(attr[i + 1]);
  }
  return true;
}

bool XMLObj::
xml_end(const char *el)
{
  return true;
}

void XMLObj::
xml_handle_data(const char *s, int len)
{
  data = string(s, len);
}

string& XMLObj::
XMLObj::get_data()
{
  return data;
}

XMLObj *XMLObj::
XMLObj::get_parent()
{
  return parent;
}

void XMLObj::
add_child(string el, XMLObj *obj)
{
  children.insert(pair<string, XMLObj *>(el, obj));
}

bool XMLObj::
get_attr(string name, string& attr)
{
  map<string, string>::iterator iter = attr_map.find(name);
  if (iter == attr_map.end())
    return false;
  attr = iter->second;
  return true;
}

XMLObjIter XMLObj::
find(string name)
{
  XMLObjIter iter;
  map<string, XMLObj *>::iterator first;
  map<string, XMLObj *>::iterator last;
  first = children.find(name);
  last = children.upper_bound(name);
  iter.set(first, last);
  return iter;
}

XMLObj *XMLObj::
find_first(string name)
{
  XMLObjIter iter;
  map<string, XMLObj *>::iterator first;
  first = children.find(name);
  if (first != children.end())
    return first->second;
  return NULL;
}
static void xml_start(void *data, const char *el, const char **attr) {
  RGWXMLParser *handler = (RGWXMLParser *)data;

  if (!handler->xml_start(el, attr))
    handler->set_failure();
}

RGWXMLParser::
RGWXMLParser() : buf(NULL), buf_len(0), cur_obj(NULL), success(true)
{
}

RGWXMLParser::
~RGWXMLParser()
{
  free(buf);
  vector<XMLObj *>::iterator iter;
  for (iter = objs.begin(); iter != objs.end(); ++iter) {
    XMLObj *obj = *iter;
    delete obj;
  }
}

bool RGWXMLParser::xml_start(const char *el, const char **attr) {
  XMLObj * obj = alloc_obj(el);
  if (!obj) {
    obj = new XMLObj();
  }
  if (!obj->xml_start(cur_obj, el, attr))
    return false;
  if (cur_obj) {
    cur_obj->add_child(el, obj);
  } else {
    children.insert(pair<string, XMLObj *>(el, obj));
  }
  cur_obj = obj;

  objs.push_back(obj);
  return true;
}

static void xml_end(void *data, const char *el) {
  RGWXMLParser *handler = (RGWXMLParser *)data;

  if (!handler->xml_end(el))
    handler->set_failure();
}

bool RGWXMLParser::xml_end(const char *el) {
  XMLObj *parent_obj = cur_obj->get_parent();
  if (!cur_obj->xml_end(el))
    return false;
  cur_obj = parent_obj;
  return true;
}

static void handle_data(void *data, const char *s, int len)
{
  RGWXMLParser *handler = (RGWXMLParser *)data;

  handler->handle_data(s, len);
}

void RGWXMLParser::handle_data(const char *s, int len)
{
  cur_obj->xml_handle_data(s, len);
}


bool RGWXMLParser::init()
{
  p = XML_ParserCreate(NULL);
  if (!p) {
    return false;
  }
  XML_SetElementHandler(p, ::xml_start, ::xml_end);
  XML_SetCharacterDataHandler(p, ::handle_data);
  XML_SetUserData(p, (void *)this);
  return true;
}

bool RGWXMLParser::parse(const char *_buf, int len, int done)
{
  int pos = buf_len;
  buf = (char *)realloc(buf, buf_len + len);
  memcpy(&buf[buf_len], _buf, len);
  buf_len += len;

  success = true;
  if (!XML_Parse(p, &buf[pos], len, done)) {
    fprintf(stderr, "Parse error at line %d:\n%s\n",
	      (int)XML_GetCurrentLineNumber(p),
	      XML_ErrorString(XML_GetErrorCode(p)));
    success = false;
  }

  if (done || !success)
    XML_ParserFree(p);

  return success;
}
