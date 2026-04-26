#pragma once

// wasi:http@0.2.3 — HTTP types, incoming-handler, and outgoing-handler.
//
// Canonical WIT sources:
//   libraries/wasi/wit/0.2.3/http/types.wit
//   libraries/wasi/wit/0.2.3/http/handler.wit
//
// These C++ declarations mirror the WIT through PSIO structural
// metadata. The inline stubs return defaults and are never called
// at runtime — psiserve's Linker wires the imports to host_function
// closures before instantiation.

#include <psio1/reflect.hpp>
#include <psio1/structural.hpp>
#include <psio1/wit_resource.hpp>

#include <wasi/0.2.3/io.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

// =====================================================================
// wasi:http/types — type aliases
// =====================================================================

// type field-key = string (deprecated alias)
// type field-name = field-key = string
using field_name  = std::string;
using field_value  = std::vector<uint8_t>;
using status_code = uint16_t;

// duration from wasi:clocks/monotonic-clock — nanoseconds as u64
using duration = uint64_t;

// =====================================================================
// wasi:http/types — method variant
// =====================================================================

struct method
{
   enum tag_t : uint8_t
   {
      get,
      head,
      post,
      put,
      delete_,
      connect,
      options,
      trace,
      patch,
      other
   };
   tag_t       tag = get;
   std::string other_value;  // valid when tag == other
};
PSIO1_REFLECT(method, tag, other_value)

// =====================================================================
// wasi:http/types — scheme variant
// =====================================================================

struct scheme
{
   enum tag_t : uint8_t
   {
      http,
      https,
      other
   };
   tag_t       tag = http;
   std::string other_value;  // valid when tag == other
};
PSIO1_REFLECT(scheme, tag, other_value)

// =====================================================================
// wasi:http/types — header-error variant
// =====================================================================

struct header_error
{
   enum tag_t : uint8_t
   {
      invalid_syntax,
      forbidden,
      immutable
   };
   tag_t tag = invalid_syntax;
};
PSIO1_REFLECT(header_error, tag)

// =====================================================================
// wasi:http/types — DNS-error-payload record
// =====================================================================

struct dns_error_payload
{
   std::optional<std::string> rcode;
   std::optional<uint16_t>    info_code;
};
PSIO1_REFLECT(dns_error_payload, rcode, info_code)

// =====================================================================
// wasi:http/types — TLS-alert-received-payload record
// =====================================================================

struct tls_alert_received_payload
{
   std::optional<uint8_t>     alert_id;
   std::optional<std::string> alert_message;
};
PSIO1_REFLECT(tls_alert_received_payload, alert_id, alert_message)

// =====================================================================
// wasi:http/types — field-size-payload record
// =====================================================================

struct field_size_payload
{
   std::optional<std::string> field_name;
   std::optional<uint32_t>    field_size;
};
PSIO1_REFLECT(field_size_payload, field_name, field_size)

// =====================================================================
// wasi:http/types — error-code variant
// =====================================================================

struct http_error_code
{
   enum tag_t : uint8_t
   {
      dns_timeout,
      dns_error,
      destination_not_found,
      destination_unavailable,
      destination_ip_prohibited,
      destination_ip_unroutable,
      connection_refused,
      connection_terminated,
      connection_timeout,
      connection_read_timeout,
      connection_write_timeout,
      connection_limit_reached,
      tls_protocol_error,
      tls_certificate_error,
      tls_alert_received,
      http_request_denied,
      http_request_length_required,
      http_request_body_size,
      http_request_method_invalid,
      http_request_uri_invalid,
      http_request_uri_too_long,
      http_request_header_section_size,
      http_request_header_size,
      http_request_trailer_section_size,
      http_request_trailer_size,
      http_response_incomplete,
      http_response_header_section_size,
      http_response_header_size,
      http_response_body_size,
      http_response_trailer_section_size,
      http_response_trailer_size,
      http_response_transfer_coding,
      http_response_content_coding,
      http_response_timeout,
      http_upgrade_failed,
      http_protocol_error,
      loop_detected,
      configuration_error,
      internal_error
   };
   tag_t tag = internal_error;

   // Payloads — valid only for corresponding tags
   dns_error_payload                  dns_error_payload_;
   field_size_payload                 field_size_payload_;
   tls_alert_received_payload         tls_alert_received_payload_;
   std::optional<uint64_t>            body_size;          // http_request_body_size, http_response_body_size
   std::optional<uint32_t>            section_size;       // various *_section_size tags
   std::optional<std::string>         string_payload;     // *_transfer_coding, *_content_coding, internal_error
};
PSIO1_REFLECT(http_error_code,
             tag,
             dns_error_payload_,
             field_size_payload_,
             tls_alert_received_payload_,
             body_size,
             section_size,
             string_payload)

// =====================================================================
// wasi:http/types — result helpers
// =====================================================================

template <typename T>
struct http_result
{
   bool              is_ok = false;
   std::optional<T>  ok;
};

struct http_result_void
{
   bool is_ok = false;
};

template <typename T>
struct http_result_error_code
{
   bool              is_ok = false;
   std::optional<T>  ok;
   http_error_code   err{};
};

struct http_result_void_error_code
{
   bool            is_ok = false;
   http_error_code err{};
};

struct http_result_void_header_error
{
   bool         is_ok = false;
   header_error err{};
};

// =====================================================================
// wasi:http/types — resource types
// =====================================================================

struct fields : psio1::wit_resource {};
PSIO1_REFLECT(fields)

// headers and trailers are aliases for fields
using headers  = fields;
using trailers = fields;

struct incoming_request : psio1::wit_resource {};
PSIO1_REFLECT(incoming_request)

struct outgoing_request : psio1::wit_resource {};
PSIO1_REFLECT(outgoing_request)

struct request_options : psio1::wit_resource {};
PSIO1_REFLECT(request_options)

struct response_outparam : psio1::wit_resource {};
PSIO1_REFLECT(response_outparam)

struct incoming_response : psio1::wit_resource {};
PSIO1_REFLECT(incoming_response)

struct incoming_body : psio1::wit_resource {};
PSIO1_REFLECT(incoming_body)

struct future_trailers : psio1::wit_resource {};
PSIO1_REFLECT(future_trailers)

struct outgoing_response : psio1::wit_resource {};
PSIO1_REFLECT(outgoing_response)

struct outgoing_body : psio1::wit_resource {};
PSIO1_REFLECT(outgoing_body)

struct future_incoming_response : psio1::wit_resource {};
PSIO1_REFLECT(future_incoming_response)

// =====================================================================
// Interface: wasi:http/types
// =====================================================================

struct wasi_http_types
{
   // -- free function --

   // http-error-code: func(err: borrow<io-error>) -> option<error-code>
   static inline std::optional<http_error_code> http_error_code_fn(
       psio1::borrow<io_error> /*err*/)
   {
      return std::nullopt;
   }

   // -- fields resource methods --

   // [constructor] fields
   static inline psio1::own<fields> fields_constructor()
   {
      return psio1::own<fields>{0};
   }

   // [static] fields.from-list: func(entries: list<tuple<field-name, field-value>>)
   //   -> result<fields, header-error>
   static inline http_result<psio1::own<fields>> fields_from_list(
       std::vector<std::tuple<field_name, field_value>> /*entries*/)
   {
      return {};
   }

   // [method] fields.get: func(name: field-name) -> list<field-value>
   static inline std::vector<field_value> fields_get(
       psio1::borrow<fields> /*self*/,
       field_name /*name*/)
   {
      return {};
   }

   // [method] fields.has: func(name: field-name) -> bool
   static inline bool fields_has(
       psio1::borrow<fields> /*self*/,
       field_name /*name*/)
   {
      return false;
   }

   // [method] fields.set: func(name: field-name, value: list<field-value>)
   //   -> result<_, header-error>
   static inline http_result_void_header_error fields_set(
       psio1::borrow<fields> /*self*/,
       field_name /*name*/,
       std::vector<field_value> /*value*/)
   {
      return {};
   }

   // [method] fields.delete: func(name: field-name) -> result<_, header-error>
   static inline http_result_void_header_error fields_delete(
       psio1::borrow<fields> /*self*/,
       field_name /*name*/)
   {
      return {};
   }

   // [method] fields.append: func(name: field-name, value: field-value)
   //   -> result<_, header-error>
   static inline http_result_void_header_error fields_append(
       psio1::borrow<fields> /*self*/,
       field_name /*name*/,
       field_value /*value*/)
   {
      return {};
   }

   // [method] fields.entries: func() -> list<tuple<field-name, field-value>>
   static inline std::vector<std::tuple<field_name, field_value>> fields_entries(
       psio1::borrow<fields> /*self*/)
   {
      return {};
   }

   // [method] fields.clone: func() -> fields
   static inline psio1::own<fields> fields_clone(
       psio1::borrow<fields> /*self*/)
   {
      return psio1::own<fields>{0};
   }

   // -- incoming-request resource methods --

   // [method] incoming-request.method: func() -> method
   static inline method incoming_request_method(
       psio1::borrow<incoming_request> /*self*/)
   {
      return {};
   }

   // [method] incoming-request.path-with-query: func() -> option<string>
   static inline std::optional<std::string> incoming_request_path_with_query(
       psio1::borrow<incoming_request> /*self*/)
   {
      return std::nullopt;
   }

   // [method] incoming-request.scheme: func() -> option<scheme>
   static inline std::optional<scheme> incoming_request_scheme(
       psio1::borrow<incoming_request> /*self*/)
   {
      return std::nullopt;
   }

   // [method] incoming-request.authority: func() -> option<string>
   static inline std::optional<std::string> incoming_request_authority(
       psio1::borrow<incoming_request> /*self*/)
   {
      return std::nullopt;
   }

   // [method] incoming-request.headers: func() -> headers
   static inline psio1::own<headers> incoming_request_headers(
       psio1::borrow<incoming_request> /*self*/)
   {
      return psio1::own<headers>{0};
   }

   // [method] incoming-request.consume: func() -> result<incoming-body>
   static inline http_result<psio1::own<incoming_body>> incoming_request_consume(
       psio1::borrow<incoming_request> /*self*/)
   {
      return {};
   }

   // -- outgoing-request resource methods --

   // [constructor] outgoing-request(headers: headers)
   static inline psio1::own<outgoing_request> outgoing_request_constructor(
       psio1::own<headers> /*headers*/)
   {
      return psio1::own<outgoing_request>{0};
   }

   // [method] outgoing-request.body: func() -> result<outgoing-body>
   static inline http_result<psio1::own<outgoing_body>> outgoing_request_body(
       psio1::borrow<outgoing_request> /*self*/)
   {
      return {};
   }

   // [method] outgoing-request.method: func() -> method
   static inline method outgoing_request_method(
       psio1::borrow<outgoing_request> /*self*/)
   {
      return {};
   }

   // [method] outgoing-request.set-method: func(method: method) -> result
   static inline http_result_void outgoing_request_set_method(
       psio1::borrow<outgoing_request> /*self*/,
       method /*method*/)
   {
      return {};
   }

   // [method] outgoing-request.path-with-query: func() -> option<string>
   static inline std::optional<std::string> outgoing_request_path_with_query(
       psio1::borrow<outgoing_request> /*self*/)
   {
      return std::nullopt;
   }

   // [method] outgoing-request.set-path-with-query: func(path-with-query: option<string>) -> result
   static inline http_result_void outgoing_request_set_path_with_query(
       psio1::borrow<outgoing_request> /*self*/,
       std::optional<std::string> /*path_with_query*/)
   {
      return {};
   }

   // [method] outgoing-request.scheme: func() -> option<scheme>
   static inline std::optional<scheme> outgoing_request_scheme(
       psio1::borrow<outgoing_request> /*self*/)
   {
      return std::nullopt;
   }

   // [method] outgoing-request.set-scheme: func(scheme: option<scheme>) -> result
   static inline http_result_void outgoing_request_set_scheme(
       psio1::borrow<outgoing_request> /*self*/,
       std::optional<scheme> /*scheme*/)
   {
      return {};
   }

   // [method] outgoing-request.authority: func() -> option<string>
   static inline std::optional<std::string> outgoing_request_authority(
       psio1::borrow<outgoing_request> /*self*/)
   {
      return std::nullopt;
   }

   // [method] outgoing-request.set-authority: func(authority: option<string>) -> result
   static inline http_result_void outgoing_request_set_authority(
       psio1::borrow<outgoing_request> /*self*/,
       std::optional<std::string> /*authority*/)
   {
      return {};
   }

   // [method] outgoing-request.headers: func() -> headers
   static inline psio1::own<headers> outgoing_request_headers(
       psio1::borrow<outgoing_request> /*self*/)
   {
      return psio1::own<headers>{0};
   }

   // -- request-options resource methods --

   // [constructor] request-options
   static inline psio1::own<request_options> request_options_constructor()
   {
      return psio1::own<request_options>{0};
   }

   // [method] request-options.connect-timeout: func() -> option<duration>
   static inline std::optional<duration> request_options_connect_timeout(
       psio1::borrow<request_options> /*self*/)
   {
      return std::nullopt;
   }

   // [method] request-options.set-connect-timeout: func(duration: option<duration>) -> result
   static inline http_result_void request_options_set_connect_timeout(
       psio1::borrow<request_options> /*self*/,
       std::optional<duration> /*duration*/)
   {
      return {};
   }

   // [method] request-options.first-byte-timeout: func() -> option<duration>
   static inline std::optional<duration> request_options_first_byte_timeout(
       psio1::borrow<request_options> /*self*/)
   {
      return std::nullopt;
   }

   // [method] request-options.set-first-byte-timeout: func(duration: option<duration>) -> result
   static inline http_result_void request_options_set_first_byte_timeout(
       psio1::borrow<request_options> /*self*/,
       std::optional<duration> /*duration*/)
   {
      return {};
   }

   // [method] request-options.between-bytes-timeout: func() -> option<duration>
   static inline std::optional<duration> request_options_between_bytes_timeout(
       psio1::borrow<request_options> /*self*/)
   {
      return std::nullopt;
   }

   // [method] request-options.set-between-bytes-timeout: func(duration: option<duration>) -> result
   static inline http_result_void request_options_set_between_bytes_timeout(
       psio1::borrow<request_options> /*self*/,
       std::optional<duration> /*duration*/)
   {
      return {};
   }

   // -- response-outparam resource methods --

   // [static] response-outparam.set: func(param: response-outparam,
   //   response: result<outgoing-response, error-code>)
   static inline void response_outparam_set(
       psio1::own<response_outparam> /*param*/,
       http_result_error_code<psio1::own<outgoing_response>> /*response*/)
   {
   }

   // -- incoming-response resource methods --

   // [method] incoming-response.status: func() -> status-code
   static inline status_code incoming_response_status(
       psio1::borrow<incoming_response> /*self*/)
   {
      return 0;
   }

   // [method] incoming-response.headers: func() -> headers
   static inline psio1::own<headers> incoming_response_headers(
       psio1::borrow<incoming_response> /*self*/)
   {
      return psio1::own<headers>{0};
   }

   // [method] incoming-response.consume: func() -> result<incoming-body>
   static inline http_result<psio1::own<incoming_body>> incoming_response_consume(
       psio1::borrow<incoming_response> /*self*/)
   {
      return {};
   }

   // -- incoming-body resource methods --

   // [method] incoming-body.stream: func() -> result<input-stream>
   static inline http_result<psio1::own<input_stream>> incoming_body_stream(
       psio1::borrow<incoming_body> /*self*/)
   {
      return {};
   }

   // [static] incoming-body.finish: func(this: incoming-body) -> future-trailers
   static inline psio1::own<future_trailers> incoming_body_finish(
       psio1::own<incoming_body> /*this_*/)
   {
      return psio1::own<future_trailers>{0};
   }

   // -- future-trailers resource methods --

   // [method] future-trailers.subscribe: func() -> pollable
   static inline psio1::own<pollable> future_trailers_subscribe(
       psio1::borrow<future_trailers> /*self*/)
   {
      return psio1::own<pollable>{0};
   }

   // [method] future-trailers.get: func()
   //   -> option<result<result<option<trailers>, error-code>>>
   // Flattened: outer option signals readiness, outer result for one-shot,
   // inner result for success/error-code, inner option for trailers presence.
   static inline std::optional<http_result<http_result_error_code<std::optional<psio1::own<trailers>>>>>
   future_trailers_get(psio1::borrow<future_trailers> /*self*/)
   {
      return std::nullopt;
   }

   // -- outgoing-response resource methods --

   // [constructor] outgoing-response(headers: headers)
   static inline psio1::own<outgoing_response> outgoing_response_constructor(
       psio1::own<headers> /*headers*/)
   {
      return psio1::own<outgoing_response>{0};
   }

   // [method] outgoing-response.status-code: func() -> status-code
   static inline status_code outgoing_response_status_code(
       psio1::borrow<outgoing_response> /*self*/)
   {
      return 200;
   }

   // [method] outgoing-response.set-status-code: func(status-code: status-code) -> result
   static inline http_result_void outgoing_response_set_status_code(
       psio1::borrow<outgoing_response> /*self*/,
       status_code /*status_code*/)
   {
      return {};
   }

   // [method] outgoing-response.headers: func() -> headers
   static inline psio1::own<headers> outgoing_response_headers(
       psio1::borrow<outgoing_response> /*self*/)
   {
      return psio1::own<headers>{0};
   }

   // [method] outgoing-response.body: func() -> result<outgoing-body>
   static inline http_result<psio1::own<outgoing_body>> outgoing_response_body(
       psio1::borrow<outgoing_response> /*self*/)
   {
      return {};
   }

   // -- outgoing-body resource methods --

   // [method] outgoing-body.write: func() -> result<output-stream>
   static inline http_result<psio1::own<output_stream>> outgoing_body_write(
       psio1::borrow<outgoing_body> /*self*/)
   {
      return {};
   }

   // [static] outgoing-body.finish: func(this: outgoing-body, trailers: option<trailers>)
   //   -> result<_, error-code>
   static inline http_result_void_error_code outgoing_body_finish(
       psio1::own<outgoing_body> /*this_*/,
       std::optional<psio1::own<trailers>> /*trailers*/)
   {
      return {};
   }

   // -- future-incoming-response resource methods --

   // [method] future-incoming-response.subscribe: func() -> pollable
   static inline psio1::own<pollable> future_incoming_response_subscribe(
       psio1::borrow<future_incoming_response> /*self*/)
   {
      return psio1::own<pollable>{0};
   }

   // [method] future-incoming-response.get: func()
   //   -> option<result<result<incoming-response, error-code>>>
   static inline std::optional<http_result<http_result_error_code<psio1::own<incoming_response>>>>
   future_incoming_response_get(psio1::borrow<future_incoming_response> /*self*/)
   {
      return std::nullopt;
   }
};

// =====================================================================
// Interface: wasi:http/incoming-handler
// =====================================================================

struct wasi_http_incoming_handler
{
   // handle: func(request: incoming-request, response-out: response-outparam)
   static inline void handle(
       psio1::own<incoming_request> /*request*/,
       psio1::own<response_outparam> /*response_out*/)
   {
   }
};

// =====================================================================
// Interface: wasi:http/outgoing-handler
// =====================================================================

struct wasi_http_outgoing_handler
{
   // handle: func(request: outgoing-request, options: option<request-options>)
   //   -> result<future-incoming-response, error-code>
   static inline http_result_error_code<psio1::own<future_incoming_response>> handle(
       psio1::own<outgoing_request> /*request*/,
       std::optional<psio1::own<request_options>> /*options*/)
   {
      return {};
   }
};

// =====================================================================
// Package and interface registration
// =====================================================================

PSIO1_PACKAGE(wasi_http, "0.2.3");
#undef  PSIO1_CURRENT_PACKAGE_
#define PSIO1_CURRENT_PACKAGE_ PSIO1_PACKAGE_TYPE_(wasi_http)

PSIO1_INTERFACE(wasi_http_types,
               types(method,
                     scheme,
                     header_error,
                     dns_error_payload,
                     tls_alert_received_payload,
                     field_size_payload,
                     http_error_code,
                     fields,
                     incoming_request,
                     outgoing_request,
                     request_options,
                     response_outparam,
                     incoming_response,
                     incoming_body,
                     future_trailers,
                     outgoing_response,
                     outgoing_body,
                     future_incoming_response),
               funcs(func(http_error_code_fn, err),
                     func(fields_constructor),
                     func(fields_from_list, entries),
                     func(fields_get, self, name),
                     func(fields_has, self, name),
                     func(fields_set, self, name, value),
                     func(fields_delete, self, name),
                     func(fields_append, self, name, value),
                     func(fields_entries, self),
                     func(fields_clone, self),
                     func(incoming_request_method, self),
                     func(incoming_request_path_with_query, self),
                     func(incoming_request_scheme, self),
                     func(incoming_request_authority, self),
                     func(incoming_request_headers, self),
                     func(incoming_request_consume, self),
                     func(outgoing_request_constructor, headers),
                     func(outgoing_request_body, self),
                     func(outgoing_request_method, self),
                     func(outgoing_request_set_method, self, method),
                     func(outgoing_request_path_with_query, self),
                     func(outgoing_request_set_path_with_query, self, path_with_query),
                     func(outgoing_request_scheme, self),
                     func(outgoing_request_set_scheme, self, scheme),
                     func(outgoing_request_authority, self),
                     func(outgoing_request_set_authority, self, authority),
                     func(outgoing_request_headers, self),
                     func(request_options_constructor),
                     func(request_options_connect_timeout, self),
                     func(request_options_set_connect_timeout, self, duration),
                     func(request_options_first_byte_timeout, self),
                     func(request_options_set_first_byte_timeout, self, duration),
                     func(request_options_between_bytes_timeout, self),
                     func(request_options_set_between_bytes_timeout, self, duration),
                     func(response_outparam_set, param, response),
                     func(incoming_response_status, self),
                     func(incoming_response_headers, self),
                     func(incoming_response_consume, self),
                     func(incoming_body_stream, self),
                     func(incoming_body_finish, this_),
                     func(future_trailers_subscribe, self),
                     func(future_trailers_get, self),
                     func(outgoing_response_constructor, headers),
                     func(outgoing_response_status_code, self),
                     func(outgoing_response_set_status_code, self, status_code),
                     func(outgoing_response_headers, self),
                     func(outgoing_response_body, self),
                     func(outgoing_body_write, self),
                     func(outgoing_body_finish, this_, trailers),
                     func(future_incoming_response_subscribe, self),
                     func(future_incoming_response_get, self)))

PSIO1_INTERFACE(wasi_http_incoming_handler,
               types(),
               funcs(func(handle, request, response_out)))

PSIO1_INTERFACE(wasi_http_outgoing_handler,
               types(),
               funcs(func(handle, request, options)))
