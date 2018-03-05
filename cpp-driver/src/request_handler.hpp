/*
  Copyright (c) DataStax, Inc.

  Licensed under the Apache License, Version 2.0 (the "License");
  you may not use this file except in compliance with the License.
  You may obtain a copy of the License at

  http://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.
*/

#ifndef __CASS_REQUEST_HANDLER_HPP_INCLUDED__
#define __CASS_REQUEST_HANDLER_HPP_INCLUDED__

#include "constants.hpp"
#include "error_response.hpp"
#include "future.hpp"
#include "request_callback.hpp"
#include "host.hpp"
#include "load_balancing.hpp"
#include "metadata.hpp"
#include "prepare_request.hpp"
#include "request.hpp"
#include "response.hpp"
#include "result_response.hpp"
#include "retry_policy.hpp"
#include "scoped_ptr.hpp"
#include "small_vector.hpp"
#include "speculative_execution.hpp"
#include "string.hpp"

#include <uv.h>

namespace cass {

class Config;
class Connection;
class ConnectionPoolManager;
class Pool;
class ExecutionProfile;
class Timer;
class TokenMap;

class ResponseFuture : public Future {
public:
  typedef SharedRefPtr<ResponseFuture> Ptr;

  ResponseFuture()
    : Future(FUTURE_TYPE_RESPONSE) { }

  ResponseFuture(const Metadata::SchemaSnapshot& schema_metadata)
    : Future(FUTURE_TYPE_RESPONSE)
    , schema_metadata(Memory::allocate<Metadata::SchemaSnapshot>(schema_metadata)) { }

  bool set_response(Address address, const Response::Ptr& response) {
    ScopedMutex lock(&mutex_);
    if (!is_set()) {
      address_ = address;
      response_ = response;
      internal_set(lock);
      return true;
    }
    return false;
  }

  const Response::Ptr& response() {
    ScopedMutex lock(&mutex_);
    internal_wait(lock);
    return response_;
  }

  bool set_error_with_address(Address address, CassError code, const String& message) {
    ScopedMutex lock(&mutex_);
    if (!is_set()) {
      address_ = address;
      internal_set_error(code, message, lock);
      return true;
    }
    return false;
  }

  bool set_error_with_response(Address address, const Response::Ptr& response,
                               CassError code, const String& message) {
    ScopedMutex lock(&mutex_);
    if (!is_set()) {
      address_ = address;
      response_ = response;
      internal_set_error(code, message, lock);
      return true;
    }
    return false;
  }

  Address address() {
    ScopedMutex lock(&mutex_);
    internal_wait(lock);
    return address_;
  }

  // Currently, used for testing only, but it could be exposed in the future.
  AddressVec attempted_addresses() {
    ScopedMutex lock(&mutex_);
    internal_wait(lock);
    return attempted_addresses_;
  }

  PrepareRequest::ConstPtr prepare_request;
  ScopedPtr<Metadata::SchemaSnapshot> schema_metadata;

private:
  friend class RequestHandler;

  void add_attempted_address(const Address& address) {
    ScopedMutex lock(&mutex_);
    attempted_addresses_.push_back(address);
  }

private:
  Address address_;
  Response::Ptr response_;
  AddressVec attempted_addresses_;
};

class RequestExecution;
class RequestListener;
class PreparedMetadata;

class RequestHandler : public RefCounted<RequestHandler> {
  friend class Memory;

public:
  typedef SharedRefPtr<RequestHandler> Ptr;

  // TODO: Remove default parameters where possible
  RequestHandler(const Request::ConstPtr& request,
                 const ResponseFuture::Ptr& future,
                 ConnectionPoolManager* manager = NULL,
                 Metrics* metrics = NULL,
                 RequestListener* listener = NULL,
                 const Address* preferred_address = NULL);
  ~RequestHandler();

  void init(const Config& config, const ExecutionProfile& profile,
            const String& connected_keyspace, const TokenMap* token_map,
            const PreparedMetadata& prepared_metdata);

  void execute();

  const RequestWrapper& wrapper() const { return wrapper_; }
  const Request* request() const { return wrapper_.request().get(); }
  CassConsistency consistency() const { return wrapper_.consistency(); }
  const Address& preferred_address() const { return preferred_address_; }

public:
  class Protected {
    friend class RequestExecution;
    Protected() {}
    Protected(Protected const&) {}
  };

  void retry(RequestExecution* request_execution, Protected);

  Host::Ptr next_host(Protected);
  int64_t next_execution(const Host::Ptr& current_host, Protected);

  void start_request(uv_loop_t* loop, Protected);

  void add_attempted_address(const Address& address, Protected);

  void notify_result_metadata_changed(const String& prepared_id,
                                      const String& query,
                                      const String& keyspace,
                                      const String& result_metadata_id,
                                      const ResultResponse::ConstPtr& result_response, Protected);

  void notify_keyspace_changed(const String& keyspace);

  bool wait_for_schema_agreement(const Host::Ptr& current_host,
                                 const Response::Ptr& response);

  bool prepare_all(const Host::Ptr& current_host,
                   const Response::Ptr& response);

  void set_response(const Host::Ptr& host,
                    const Response::Ptr& response);
  void set_error(CassError code, const String& message);
  void set_error(const Host::Ptr& host,
                 CassError code, const String& message);
  void set_error_with_error_response(const Host::Ptr& host,
                                     const Response::Ptr& error,
                                     CassError code, const String& message);

private:
  static void on_timeout(Timer* timer);

private:
  void stop_request();
  void internal_retry(RequestExecution* request_execution);

private:
  RequestWrapper wrapper_;
  SharedRefPtr<ResponseFuture> future_;

  Atomic<bool> is_canceled_;
  Atomic<int> running_executions_;

  uv_mutex_t lock_;
  ScopedPtr<QueryPlan> query_plan_;
  ScopedPtr<SpeculativeExecutionPlan> execution_plan_;
  Timer timer_;
  bool is_timer_started_;
  uv_thread_t timer_thread_id_;

  const uint64_t start_time_ns_;
  Metrics* const metrics_;
  RequestListener* const listener_;
  ConnectionPoolManager* const manager_;
  const Address preferred_address_;
};

class RequestListener {
public:
  virtual void on_result_metadata_changed(const String& prepared_id,
                                          const String& query,
                                          const String& keyspace,
                                          const String& result_metadata_id,
                                          const ResultResponse::ConstPtr& result_response) = 0;

  virtual void on_keyspace_changed(const String& keyspace) = 0;

  virtual bool on_wait_for_schema_agreement(const RequestHandler::Ptr& request_handler,
                                            const Host::Ptr& current_host,
                                            const Response::Ptr& response) = 0;

  virtual bool on_prepare_all(const RequestHandler::Ptr& request_handler,
                              const Host::Ptr& current_host,
                              const Response::Ptr& response) = 0;
};

class RequestExecution : public RequestCallback {
public:
  typedef SharedRefPtr<RequestExecution> Ptr;

  RequestExecution(RequestHandler* request_handler);

  const Host::Ptr& current_host() const { return current_host_; }
  void next_host() {
    current_host_ = request_handler_->next_host(RequestHandler::Protected());
  }

  void notify_result_metadata_changed(const Request *request,
                                      ResultResponse* result_response);

  virtual void on_retry_current_host();
  virtual void on_retry_next_host();

private:
  static void on_execute_next(Timer* timer);

  void retry_current_host();
  void retry_next_host();

  virtual void on_write(Connection* connection);

  virtual void on_set(ResponseMessage* response);
  virtual void on_error(CassError code, const String& message);

  void on_result_response(Connection* connection, ResponseMessage* response);
  void on_error_response(Connection* connection, ResponseMessage* response);
  void on_error_unprepared(Connection* connection, ErrorResponse* error);

private:
  void set_response(const Response::Ptr& response);
  void set_error(CassError code, const String& message);
  void set_error_with_error_response(const Response::Ptr& error,
                                     CassError code, const String& message);

private:
  RequestHandler::Ptr request_handler_;
  Host::Ptr current_host_;
  Connection* connection_;
  Timer schedule_timer_;
  int num_retries_;
  const uint64_t start_time_ns_;
};

} // namespace cass

#endif
