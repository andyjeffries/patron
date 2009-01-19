#include <ruby.h>
#include <curl/curl.h>

static VALUE mPatron = Qnil;
static VALUE cSession = Qnil;
static VALUE ePatronError = Qnil;
static VALUE eUnsupportedProtocol = Qnil;
static VALUE eURLFormatError = Qnil;
static VALUE eHostResolutionError = Qnil;
static VALUE eConnectionFailed = Qnil;
static VALUE ePartialFileError = Qnil;
static VALUE eTimeoutError = Qnil;
static VALUE eTooManyRedirects = Qnil;


struct curl_state {
  CURL* handle;
  char error_buf[CURL_ERROR_SIZE];
  struct curl_slist* headers;
};


//------------------------------------------------------------------------------
// Curl Callbacks
//

// Takes data streamed from libcurl and writes it to a Ruby string buffer.
static size_t session_write_handler(char* stream, size_t size, size_t nmemb, VALUE out) {
  rb_str_buf_cat(out, stream, size * nmemb);
  return size * nmemb;
}

static size_t session_header_handler(char* stream, size_t size, size_t nmemb, VALUE out) {
  rb_str_buf_cat(out, stream, size * nmemb);
  return size * nmemb;
}

// static size_t session_read_shim(char* stream, size_t size, size_t nmemb, VALUE proc) {
//   size_t result = size * nmemb;
//   VALUE string = rb_funcall(proc, rb_intern("call"), 1, result);
//   size_t len = RSTRING(string)->len;
//   memcpy(stream, RSTRING(string)->ptr, len);
//   return len;
// }
 
//------------------------------------------------------------------------------
// Object allocation
//

// Cleans up the Curl handle when the Session object is garbage collected.
void session_free(struct curl_state *curl) {
  curl_easy_cleanup(curl->handle);
  free(curl);
}

// Allocates curl_state data needed for a new Session object.
VALUE session_alloc(VALUE klass) {
  struct curl_state* curl;
  VALUE obj = Data_Make_Struct(klass, struct curl_state, NULL, session_free, curl);
  return obj;
}


//------------------------------------------------------------------------------
// Method implementations
//

// Returns the version of the embedded libcurl as a string.
VALUE libcurl_version(VALUE klass) {
  char* value = curl_version();
  return rb_str_new2(value);
}

// Initializes the libcurl handle on object initialization.
// NOTE: This must be called from Session#initialize.
VALUE session_ext_initialize(VALUE self) {
  struct curl_state *state;
  Data_Get_Struct(self, struct curl_state, state);

  state->handle = curl_easy_init();

  return self;
}

// URL escapes the provided string.
VALUE session_escape(VALUE self, VALUE value) {
  struct curl_state *state;
  Data_Get_Struct(self, struct curl_state, state);

  VALUE string = StringValue(value);
  char* escaped = curl_easy_escape(state->handle,
                                   RSTRING(string)->ptr,
                                   RSTRING(string)->len);

  VALUE retval = rb_str_new2(escaped);
  curl_free(escaped);

  return retval;
}

// Unescapes the provided string.
VALUE session_unescape(VALUE self, VALUE value) {
  struct curl_state *state;
  Data_Get_Struct(self, struct curl_state, state);

  VALUE string = StringValue(value);
  char* unescaped = curl_easy_unescape(state->handle,
                                       RSTRING(string)->ptr,
                                       RSTRING(string)->len,
                                       NULL);

  VALUE retval = rb_str_new2(unescaped);
  curl_free(unescaped);

  return retval;
}

// Callback used to iterate over the HTTP headers and store them in an slist.
static VALUE each_http_header(VALUE header, VALUE self) {
  struct curl_state *state;
  Data_Get_Struct(self, struct curl_state, state);

  VALUE name = rb_obj_as_string(rb_ary_entry(header, 0));
  VALUE value = rb_obj_as_string(rb_ary_entry(header, 1));

  VALUE header_str = Qnil;
  header_str = rb_str_plus(name, rb_str_new2(": "));
  header_str = rb_str_plus(header_str, value);

  state->headers = curl_slist_append(state->headers, StringValuePtr(header_str));
  return Qnil;
}

// Set the options on the Curl handle from a Request object. Takes each field
// in the Request object and uses it to set the appropriate option on the Curl
// handle.
void set_options_from_request(VALUE self, VALUE request) {
  struct curl_state *state;
  Data_Get_Struct(self, struct curl_state, state);

  CURL* curl = state->handle;

  ID action = SYM2ID(rb_iv_get(request, "@action"));
  if (action == rb_intern("get")) {
    curl_easy_setopt(curl, CURLOPT_HTTPGET, 1);
  } else if (action == rb_intern("post")) {
    curl_easy_setopt(curl, CURLOPT_HTTPPOST, 1);
  } else if (action == rb_intern("put")) {
    curl_easy_setopt(curl, CURLOPT_UPLOAD, 1);
  } else if (action == rb_intern("delete")) {
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");
  } else if (action == rb_intern("head")) {
    curl_easy_setopt(curl, CURLOPT_NOBODY, 1);
  }

  curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, state->error_buf);

  VALUE url = rb_iv_get(request, "@url");
  if (NIL_P(url)) {
    rb_raise(rb_eArgError, "Must provide a URL");
  }
  curl_easy_setopt(curl, CURLOPT_URL, StringValuePtr(url));

  VALUE timeout = rb_iv_get(request, "@timeout");
  if (!NIL_P(timeout)) {
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, FIX2INT(timeout));
  }

  VALUE redirects = rb_iv_get(request, "@max_redirects");
  if (!NIL_P(redirects)) {
    int r = FIX2INT(redirects);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, r == 0 ? 0 : 1);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, r);
  }

  VALUE headers = rb_iv_get(request, "@headers");
  if (!NIL_P(headers)) {
    if (rb_type(headers) != T_HASH) {
      rb_raise(rb_eArgError, "Headers must be passed in a hash.");
    }

    rb_iterate(rb_each, headers, each_http_header, self);
  }

  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, state->headers);
}

// Use the info in a Curl handle to create a new Response object.
VALUE create_response(CURL* curl) {
  VALUE response = rb_class_new_instance(0, 0,
                      rb_const_get(mPatron, rb_intern("Response")));

  char* url = NULL;
  curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &url);
  rb_iv_set(response, "@url", rb_str_new2(url));

  long code = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
  rb_iv_set(response, "@status", INT2NUM(code));

  long count = 0;
  curl_easy_getinfo(curl, CURLINFO_REDIRECT_COUNT, &count);
  rb_iv_set(response, "@redirect_count", INT2NUM(count));

  return response;
}

// Raise an exception based on the Curl error code.
VALUE select_error(CURLcode code) {
  VALUE error = Qnil;
  switch (code) {
    case CURLE_UNSUPPORTED_PROTOCOL:  error = eUnsupportedProtocol; break;
    case CURLE_URL_MALFORMAT:         error = eURLFormatError;      break;
    case CURLE_COULDNT_RESOLVE_HOST:  error = eHostResolutionError; break;
    case CURLE_COULDNT_CONNECT:       error = eConnectionFailed;    break;
    case CURLE_PARTIAL_FILE:          error = ePartialFileError;    break;
    case CURLE_OPERATION_TIMEDOUT:    error = eTimeoutError;        break;
    case CURLE_TOO_MANY_REDIRECTS:    error = eTooManyRedirects;    break;

    default: error = ePatronError;
  }

  return error;
}

// Perform the actual HTTP request by calling libcurl.
static VALUE perform_request(VALUE self) {
  struct curl_state *state;
  Data_Get_Struct(self, struct curl_state, state);

  CURL* curl = state->handle;

  // headers
  VALUE header_buffer = rb_str_buf_new(32768);
  curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, &session_header_handler);
  curl_easy_setopt(curl, CURLOPT_HEADERDATA, header_buffer);

  // body
  VALUE body_buffer = rb_str_buf_new(32768);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &session_write_handler);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, body_buffer);

  CURLcode ret = curl_easy_perform(curl);
  if (CURLE_OK == ret) {
    VALUE response = create_response(curl);
    rb_iv_set(response, "@body", body_buffer);
    rb_funcall(response, rb_intern("parse_headers"), 1, header_buffer);
    return response;
  } else {
    rb_raise(select_error(ret), state->error_buf);
  }
}

// Cleanup after each request by resetting the Curl handle and deallocating all
// request related objects such as the header slist.
static VALUE cleanup(VALUE self) {
  struct curl_state *state;
  Data_Get_Struct(self, struct curl_state, state);

  curl_easy_reset(state->handle);

  if (state->headers) {
    curl_slist_free_all(state->headers);
    state->headers = NULL;
  }

  return Qnil;
}

VALUE session_handle_request(VALUE self, VALUE request) {
  set_options_from_request(self, request);
  return rb_ensure(&perform_request, self, &cleanup, self);
}

//------------------------------------------------------------------------------
// Extension initialization
//

void Init_session_ext() {
  curl_global_init(CURL_GLOBAL_NOTHING);
  rb_require("patron/error");

  mPatron = rb_define_module("Patron");

  ePatronError = rb_const_get(mPatron, rb_intern("Error"));

  eUnsupportedProtocol = rb_const_get(mPatron, rb_intern("UnsupportedProtocol"));
  eURLFormatError = rb_const_get(mPatron, rb_intern("URLFormatError"));
  eHostResolutionError = rb_const_get(mPatron, rb_intern("HostResolutionError"));
  eConnectionFailed = rb_const_get(mPatron, rb_intern("ConnectionFailed"));
  ePartialFileError = rb_const_get(mPatron, rb_intern("PartialFileError"));
  eTimeoutError = rb_const_get(mPatron, rb_intern("TimeoutError"));
  eTooManyRedirects = rb_const_get(mPatron, rb_intern("TooManyRedirects"));

  rb_define_module_function(mPatron, "libcurl_version", libcurl_version, 0);

  cSession = rb_define_class_under(mPatron, "Session", rb_cObject);
  rb_define_alloc_func(cSession, session_alloc);

  rb_define_method(cSession, "ext_initialize", session_ext_initialize, 0);
  rb_define_method(cSession, "escape",         session_escape,         1);
  rb_define_method(cSession, "unescape",       session_unescape,       1);
  rb_define_method(cSession, "handle_request", session_handle_request, 1);
}
