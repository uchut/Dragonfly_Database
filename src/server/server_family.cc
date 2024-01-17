// Copyright 2022, DragonflyDB authors.  All rights reserved.
// See LICENSE for licensing terms.
//

#include "server/server_family.h"

#include <absl/cleanup/cleanup.h>
#include <absl/random/random.h>  // for master_id_ generation.
#include <absl/strings/match.h>
#include <absl/strings/str_join.h>
#include <absl/strings/str_replace.h>
#include <absl/strings/strip.h>
#include <croncpp.h>  // cron::cronexpr
#include <sys/resource.h>
#include <sys/utsname.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <optional>

#include "facade/error.h"
#include "slowlog.h"

extern "C" {
#include "redis/redis_aux.h"
}

#include "base/flags.h"
#include "base/logging.h"
#include "core/compact_object.h"
#include "facade/cmd_arg_parser.h"
#include "facade/dragonfly_connection.h"
#include "facade/reply_builder.h"
#include "io/file_util.h"
#include "io/proc_reader.h"
#include "search/doc_index.h"
#include "server/acl/acl_commands_def.h"
#include "server/command_registry.h"
#include "server/conn_context.h"
#include "server/debugcmd.h"
#include "server/detail/save_stages_controller.h"
#include "server/dflycmd.h"
#include "server/engine_shard_set.h"
#include "server/error.h"
#include "server/generic_family.h"
#include "server/journal/journal.h"
#include "server/main_service.h"
#include "server/memory_cmd.h"
#include "server/protocol_client.h"
#include "server/rdb_load.h"
#include "server/rdb_save.h"
#include "server/script_mgr.h"
#include "server/server_state.h"
#include "server/tiered_storage.h"
#include "server/transaction.h"
#include "server/version.h"
#include "strings/human_readable.h"
#include "util/accept_server.h"
#include "util/aws/aws.h"
#include "util/fibers/fiber_file.h"

using namespace std;

struct ReplicaOfFlag {
  string host;
  string port;

  bool has_value() const {
    return !host.empty() && !port.empty();
  }
};

static bool AbslParseFlag(std::string_view in, ReplicaOfFlag* flag, std::string* err);
static std::string AbslUnparseFlag(const ReplicaOfFlag& flag);

struct CronExprFlag {
  static constexpr std::string_view kCronPrefix = "0 "sv;
  std::optional<cron::cronexpr> cron_expr;
};

static bool AbslParseFlag(std::string_view in, CronExprFlag* flag, std::string* err);
static std::string AbslUnparseFlag(const CronExprFlag& flag);

ABSL_FLAG(string, dir, "", "working directory");
ABSL_FLAG(string, dbfilename, "dump-{timestamp}",
          "the filename to save/load the DB, instead of/with {timestamp} can be used {Y}, {m}, and "
          "{d} macros");
ABSL_FLAG(string, requirepass, "",
          "password for AUTH authentication. "
          "If empty can also be set with DFLY_PASSWORD environment variable.");
ABSL_FLAG(uint32_t, maxclients, 64000, "Maximum number of concurrent clients allowed.");

ABSL_FLAG(string, save_schedule, "", "the flag is deprecated, please use snapshot_cron instead");
ABSL_FLAG(CronExprFlag, snapshot_cron, {},
          "cron expression for the time to save a snapshot, crontab style");
ABSL_FLAG(bool, df_snapshot_format, true,
          "if true, save in dragonfly-specific snapshotting format");
ABSL_FLAG(int, epoll_file_threads, 0,
          "thread size for file workers when running in epoll mode, default is hardware concurrent "
          "threads");
ABSL_FLAG(ReplicaOfFlag, replicaof, ReplicaOfFlag{},
          "Specifies a host and port which point to a target master "
          "to replicate. "
          "Format should be <IPv4>:<PORT> or host:<PORT> or [<IPv6>]:<PORT>");
ABSL_FLAG(int32_t, slowlog_log_slower_than, 10000,
          "Add commands slower than this threshold to slow log. The value is expressed in "
          "microseconds and if it's negative - disables the slowlog.");
ABSL_FLAG(uint32_t, slowlog_max_len, 20, "Slow log maximum length.");

ABSL_FLAG(string, s3_endpoint, "", "endpoint for s3 snapshots, default uses aws regional endpoint");
ABSL_FLAG(bool, s3_use_https, true, "whether to use https for s3 endpoints");
// Disable EC2 metadata by default, or if a users credentials are invalid the
// AWS client will spent 30s trying to connect to inaccessable EC2 endpoints
// to load the credentials.
ABSL_FLAG(bool, s3_ec2_metadata, false,
          "whether to load credentials and configuration from EC2 metadata");
// Enables S3 payload signing over HTTP. This reduces the latency and resource
// usage when writing snapshots to S3, at the expense of security.
ABSL_FLAG(bool, s3_sign_payload, true,
          "whether to sign the s3 request payload when uploading snapshots");

ABSL_DECLARE_FLAG(int32_t, port);
ABSL_DECLARE_FLAG(bool, cache_mode);
ABSL_DECLARE_FLAG(uint32_t, hz);
ABSL_DECLARE_FLAG(bool, tls);
ABSL_DECLARE_FLAG(string, tls_ca_cert_file);
ABSL_DECLARE_FLAG(string, tls_ca_cert_dir);

bool AbslParseFlag(std::string_view in, ReplicaOfFlag* flag, std::string* err) {
#define RETURN_ON_ERROR(cond, m)                                           \
  do {                                                                     \
    if ((cond)) {                                                          \
      *err = m;                                                            \
      LOG(WARNING) << "Error in parsing arguments for --replicaof: " << m; \
      return false;                                                        \
    }                                                                      \
  } while (0)

  if (in.empty()) {  // on empty flag "parse" nothing. If we return false then DF exists.
    *flag = ReplicaOfFlag{};
    return true;
  }

  auto pos = in.find_last_of(':');
  RETURN_ON_ERROR(pos == string::npos, "missing ':'.");

  string_view ip = in.substr(0, pos);
  flag->port = in.substr(pos + 1);

  RETURN_ON_ERROR(ip.empty() || flag->port.empty(), "IP/host or port are empty.");

  // For IPv6: ip1.front == '[' AND ip1.back == ']'
  // For IPv4: ip1.front != '[' AND ip1.back != ']'
  // Together, this ip1.front == '[' iff ip1.back == ']', which can be implemented as XNOR (NOT XOR)
  RETURN_ON_ERROR(((ip.front() == '[') ^ (ip.back() == ']')), "unclosed brackets.");

  if (ip.front() == '[') {
    // shortest possible IPv6 is '::1' (loopback)
    RETURN_ON_ERROR(ip.length() <= 2, "IPv6 host name is too short");

    flag->host = ip.substr(1, ip.length() - 2);
  } else {
    flag->host = ip;
  }

  VLOG(1) << "--replicaof: Received " << flag->host << " :  " << flag->port;
  return true;
#undef RETURN_ON_ERROR
}

std::string AbslUnparseFlag(const ReplicaOfFlag& flag) {
  return (flag.has_value()) ? absl::StrCat(flag.host, ":", flag.port) : "";
}

bool AbslParseFlag(std::string_view in, CronExprFlag* flag, std::string* err) {
  if (in.empty()) {
    flag->cron_expr = std::nullopt;
    return true;
  }
  if (absl::StartsWith(in, "\"")) {
    *err = absl::StrCat("Could it be that you put quotes in the flagfile?");

    return false;
  }

  std::string raw_cron_expr = absl::StrCat(CronExprFlag::kCronPrefix, in);
  try {
    VLOG(1) << "creating cron from: '" << raw_cron_expr << "'";
    flag->cron_expr = cron::make_cron(raw_cron_expr);
    return true;
  } catch (const cron::bad_cronexpr& ex) {
    *err = ex.what();
  }
  return false;
}

std::string AbslUnparseFlag(const CronExprFlag& flag) {
  if (flag.cron_expr) {
    auto str_expr = to_cronstr(*flag.cron_expr);
    DCHECK(absl::StartsWith(str_expr, CronExprFlag::kCronPrefix));
    return str_expr.substr(CronExprFlag::kCronPrefix.size());
  }
  return "";
}

namespace dfly {

namespace fs = std::filesystem;

using absl::GetFlag;
using absl::StrCat;
using namespace facade;
using namespace util;
using detail::SaveStagesController;
using http::StringResponse;
using strings::HumanReadableNumBytes;
using SendStatsType = facade::ReplyStats::SendStatsType;

namespace {

const auto kRedisVersion = "6.2.11";
constexpr string_view kS3Prefix = "s3://"sv;

using EngineFunc = void (ServerFamily::*)(CmdArgList args, ConnectionContext* cntx);

inline CommandId::Handler HandlerFunc(ServerFamily* se, EngineFunc f) {
  return [=](CmdArgList args, ConnectionContext* cntx) { return (se->*f)(args, cntx); };
}

using CI = CommandId;

string UnknownCmd(string cmd, CmdArgList args) {
  return absl::StrCat("unknown command '", cmd, "' with args beginning with: ",
                      StrJoin(args.begin(), args.end(), ", ", CmdArgListFormatter()));
}

bool IsCloudPath(string_view path) {
  return absl::StartsWith(path, kS3Prefix);
}

// Check that if TLS is used at least one form of client authentication is
// enabled. That means either using a password or giving a root
// certificate for authenticating client certificates which will
// be required.
bool ValidateServerTlsFlags() {
  if (!absl::GetFlag(FLAGS_tls)) {
    return true;
  }

  bool has_auth = false;

  if (!dfly::GetPassword().empty()) {
    has_auth = true;
  }

  if (!(absl::GetFlag(FLAGS_tls_ca_cert_file).empty() &&
        absl::GetFlag(FLAGS_tls_ca_cert_dir).empty())) {
    has_auth = true;
  }

  if (!has_auth) {
    LOG(ERROR) << "TLS configured but no authentication method is used!";
    return false;
  }

  return true;
}

bool IsReplicatingNoOne(string_view host, string_view port) {
  return absl::EqualsIgnoreCase(host, "no") && absl::EqualsIgnoreCase(port, "one");
}

template <typename T> void UpdateMax(T* maxv, T current) {
  *maxv = std::max(*maxv, current);
}

void SetMasterFlagOnAllThreads(bool is_master) {
  auto cb = [is_master](auto* pb) { ServerState::tlocal()->is_master = is_master; };
  shard_set->pool()->Await(cb);
}

std::optional<cron::cronexpr> InferSnapshotCronExpr() {
  string save_time = GetFlag(FLAGS_save_schedule);
  auto cron_expr = GetFlag(FLAGS_snapshot_cron);

  if (!save_time.empty()) {
    LOG(ERROR) << "save_schedule flag is deprecated, please use snapshot_cron instead";
    exit(1);
  }

  if (cron_expr.cron_expr) {
    return std::move(cron_expr.cron_expr);
  }

  return std::nullopt;
}

void SlowLogGet(dfly::CmdArgList args, dfly::ConnectionContext* cntx, dfly::Service& service,
                std::string_view sub_cmd) {
  size_t requested_slow_log_length = UINT32_MAX;
  size_t argc = args.size();
  if (argc >= 3) {
    cntx->SendError(facade::UnknownSubCmd(sub_cmd, "SLOWLOG"), facade::kSyntaxErrType);
    return;
  } else if (argc == 2) {
    string_view length = facade::ArgS(args, 1);
    int64_t num;
    if ((!absl::SimpleAtoi(length, &num)) || (num < -1)) {
      cntx->SendError("count should be greater than or equal to -1");
      return;
    }
    if (num >= 0) {
      requested_slow_log_length = num;
    }
  }

  // gather all the individual slowlogs from all the fibers and sort them by their timestamp
  std::vector<boost::circular_buffer<SlowLogEntry>> entries(service.proactor_pool().size());
  service.proactor_pool().AwaitFiberOnAll([&](auto index, auto* context) {
    auto shard_entries = ServerState::tlocal()->GetSlowLog().Entries();
    entries[index] = shard_entries;
  });
  std::vector<std::pair<SlowLogEntry, unsigned>> merged_slow_log;
  for (size_t i = 0; i < entries.size(); ++i) {
    for (const auto& log_item : entries[i]) {
      merged_slow_log.emplace_back(log_item, i);
    }
  }

  std::sort(merged_slow_log.begin(), merged_slow_log.end(), [](const auto& e1, const auto& e2) {
    return e1.first.unix_ts_usec > e2.first.unix_ts_usec;
  });

  requested_slow_log_length = std::min(merged_slow_log.size(), requested_slow_log_length);

  auto* rb = static_cast<facade::RedisReplyBuilder*>(cntx->reply_builder());
  rb->StartArray(requested_slow_log_length);
  for (size_t i = 0; i < requested_slow_log_length; ++i) {
    const auto& entry = merged_slow_log[i].first;
    const auto& args = entry.cmd_args;

    rb->StartArray(6);

    rb->SendLong(entry.entry_id * service.proactor_pool().size() + merged_slow_log[i].second);
    rb->SendLong(entry.unix_ts_usec / 1000000);
    rb->SendLong(entry.exec_time_usec);

    // if we truncated the args, there is one pseudo-element containing the number of truncated
    // args that we must add, so the result length is increased by 1
    size_t len = args.size() + int(args.size() < entry.original_length);

    rb->StartArray(len);

    for (const auto& arg : args) {
      if (arg.second > 0) {
        auto suffix = absl::StrCat("... (", arg.second, " more bytes)");
        auto cmd_arg = arg.first.substr(0, kMaximumSlowlogArgLength - suffix.length());
        rb->SendBulkString(absl::StrCat(cmd_arg, suffix));
      } else {
        rb->SendBulkString(arg.first);
      }
    }
    // if we truncated arguments - add a special string to indicate that.
    if (args.size() < entry.original_length) {
      rb->SendBulkString(
          absl::StrCat("... (", entry.original_length - args.size(), " more arguments)"));
    }

    rb->SendBulkString(entry.client_ip);
    rb->SendBulkString(entry.client_name);
  }
}

void ClientSetName(CmdArgList args, ConnectionContext* cntx) {
  if (args.size() == 1) {
    cntx->conn()->SetName(string{ArgS(args, 0)});
    return cntx->SendOk();
  } else {
    return cntx->SendError(facade::kSyntaxErr);
  }
}

void ClientGetName(CmdArgList args, ConnectionContext* cntx) {
  if (!args.empty()) {
    return cntx->SendError(facade::kSyntaxErr);
  }
  auto* rb = static_cast<RedisReplyBuilder*>(cntx->reply_builder());
  if (auto name = cntx->conn()->GetName(); !name.empty()) {
    return rb->SendBulkString(name);
  } else {
    return rb->SendNull();
  }
}

void ClientList(CmdArgList args, absl::Span<facade::Listener*> listeners, ConnectionContext* cntx) {
  if (!args.empty()) {
    return cntx->SendError(facade::kSyntaxErr);
  }

  vector<string> client_info;
  absl::base_internal::SpinLock mu;

  // we can not preempt the connection traversal, so we need to use a spinlock.
  // alternatively we could lock when mutating the connection list, but it seems not important.
  auto cb = [&](unsigned thread_index, util::Connection* conn) {
    facade::Connection* dcon = static_cast<facade::Connection*>(conn);
    string info = dcon->GetClientInfo(thread_index);
    absl::base_internal::SpinLockHolder l(&mu);
    client_info.push_back(std::move(info));
  };

  for (auto* listener : listeners) {
    listener->TraverseConnections(cb);
  }

  string result = absl::StrJoin(client_info, "\n");
  result.append("\n");
  auto* rb = static_cast<RedisReplyBuilder*>(cntx->reply_builder());
  return rb->SendVerbatimString(result);
}

void ClientPauseCmd(CmdArgList args, absl::Span<facade::Listener*> listeners,
                    ConnectionContext* cntx) {
  CmdArgParser parser(args);

  auto timeout = parser.Next<uint64_t>();
  enum ClientPause pause_state = ClientPause::ALL;
  if (parser.HasNext()) {
    pause_state = parser.ToUpper().Switch("WRITE", ClientPause::WRITE, "ALL", ClientPause::ALL);
  }
  if (auto err = parser.Error(); err) {
    return cntx->SendError(err->MakeReply());
  }

  // Set global pause state and track commands that are running when the pause state is flipped.
  // Exlude already paused commands from the busy count.
  DispatchTracker tracker{listeners, cntx->conn(), true /* ignore paused commands */};
  shard_set->pool()->Await([&tracker, pause_state](util::ProactorBase* pb) {
    // Commands don't suspend before checking the pause state, so
    // it's impossible to deadlock on waiting for a command that will be paused.
    tracker.TrackOnThread();
    ServerState::tlocal()->SetPauseState(pause_state, true);
  });

  // TODO handle blocking commands
  // Wait for all busy commands to finish running before replying to guarantee
  // that no more (write) operations will occur.
  const absl::Duration kDispatchTimeout = absl::Seconds(1);
  if (!tracker.Wait(kDispatchTimeout)) {
    LOG(WARNING) << "Couldn't wait for commands to finish dispatching in " << kDispatchTimeout;
    shard_set->pool()->Await([pause_state](util::ProactorBase* pb) {
      ServerState::tlocal()->SetPauseState(pause_state, false);
    });
    return cntx->SendError("Failed to pause all running clients");
  }

  // We should not expire/evict keys while clients are puased.
  shard_set->RunBriefInParallel(
      [](EngineShard* shard) { shard->db_slice().SetExpireAllowed(false); });

  fb2::Fiber("client_pause", [timeout, pause_state]() mutable {
    // On server shutdown we sleep 10ms to make sure all running task finish, therefore 10ms steps
    // ensure this fiber will not left hanging .
    auto step = 10ms;
    auto timeout_ms = timeout * 1ms;
    int64_t steps = timeout_ms.count() / step.count();
    ServerState& etl = *ServerState::tlocal();
    do {
      ThisFiber::SleepFor(step);
    } while (etl.gstate() != GlobalState::SHUTTING_DOWN && --steps > 0);

    if (etl.gstate() != GlobalState::SHUTTING_DOWN) {
      shard_set->pool()->AwaitFiberOnAll([pause_state](util::ProactorBase* pb) {
        ServerState::tlocal()->SetPauseState(pause_state, false);
      });
      shard_set->RunBriefInParallel(
          [](EngineShard* shard) { shard->db_slice().SetExpireAllowed(true); });
    }
  }).Detach();

  cntx->SendOk();
}

void ClientTracking(CmdArgList args, ConnectionContext* cntx) {
  if (args.size() != 1)
    return cntx->SendError(kSyntaxErr);

  auto* rb = static_cast<RedisReplyBuilder*>(cntx->reply_builder());
  if (!rb->IsResp3())
    return cntx->SendError(
        "Client tracking is currently not supported for RESP2. Please use RESP3.");

  ToUpper(&args[0]);
  string_view state = ArgS(args, 0);
  bool is_on;
  if (state == "ON") {
    is_on = true;
  } else if (state == "OFF") {
    is_on = false;
  } else {
    return cntx->SendError(kSyntaxErr);
  }

  cntx->conn()->SetClientTrackingSwitch(is_on);
  return cntx->SendOk();
}

void ClientKill(CmdArgList args, absl::Span<facade::Listener*> listeners, ConnectionContext* cntx) {
  std::function<bool(facade::Connection * conn)> evaluator;

  if (args.size() == 1) {
    string_view ip_port = ArgS(args, 0);
    if (ip_port.find(':') != ip_port.npos) {
      evaluator = [ip_port](facade::Connection* conn) {
        return conn->RemoteEndpointStr() == ip_port;
      };
    }
  } else if (args.size() == 2) {
    ToUpper(&args[0]);
    string_view filter_type = ArgS(args, 0);
    string_view filter_value = ArgS(args, 1);
    if (filter_type == "ADDR") {
      evaluator = [filter_value](facade::Connection* conn) {
        return conn->RemoteEndpointStr() == filter_value;
      };
    } else if (filter_type == "LADDR") {
      evaluator = [filter_value](facade::Connection* conn) {
        return conn->LocalBindStr() == filter_value;
      };
    } else if (filter_type == "ID") {
      uint32_t id;
      if (absl::SimpleAtoi(filter_value, &id)) {
        evaluator = [id](facade::Connection* conn) { return conn->GetClientId() == id; };
      }
    }
    // TODO: Add support for KILL USER/TYPE/SKIPME
  }

  if (!evaluator) {
    return cntx->SendError(kSyntaxErr);
  }

  const bool is_admin_request = cntx->conn()->IsPrivileged();

  atomic<uint32_t> killed_connections = 0;
  atomic<uint32_t> kill_errors = 0;
  auto cb = [&](unsigned thread_index, util::Connection* conn) {
    facade::Connection* dconn = static_cast<facade::Connection*>(conn);
    if (evaluator(dconn)) {
      if (is_admin_request || !dconn->IsPrivileged()) {
        dconn->ShutdownSelf();
        killed_connections.fetch_add(1);
      } else {
        kill_errors.fetch_add(1);
      }
    }
  };

  for (auto* listener : listeners) {
    listener->TraverseConnections(cb);
  }

  if (kill_errors.load() == 0) {
    return cntx->SendLong(killed_connections.load());
  } else {
    return cntx->SendError(absl::StrCat("Killed ", killed_connections.load(),
                                        " client(s), but unable to kill ", kill_errors.load(),
                                        " admin client(s)."));
  }
}

std::string_view GetOSString() {
  // Call uname() only once since it can be expensive. Cache the final result in a static string.
  static string os_string = []() {
    utsname os_name;
    uname(&os_name);
    return StrCat(os_name.sysname, " ", os_name.release, " ", os_name.machine);
  }();

  return os_string;
}

string_view GetRedisMode() {
  return ClusterConfig::IsEnabledOrEmulated() ? "cluster"sv : "standalone"sv;
}

}  // namespace

ServerFamily::ServerFamily(Service* service) : service_(*service) {
  start_time_ = time(NULL);
  last_save_info_.save_time = start_time_;
  script_mgr_.reset(new ScriptMgr());
  journal_.reset(new journal::Journal());

  {
    absl::InsecureBitGen eng;
    master_id_ = GetRandomHex(eng, CONFIG_RUN_ID_SIZE);
    DCHECK_EQ(CONFIG_RUN_ID_SIZE, master_id_.size());
  }

  if (auto ec =
          detail::ValidateFilename(GetFlag(FLAGS_dbfilename), GetFlag(FLAGS_df_snapshot_format));
      ec) {
    LOG(ERROR) << ec.Format();
    exit(1);
  }

  if (!ValidateServerTlsFlags()) {
    exit(1);
  }
  ValidateClientTlsFlags();
}

ServerFamily::~ServerFamily() {
}

void SetMaxClients(std::vector<facade::Listener*>& listeners, uint32_t maxclients) {
  for (auto* listener : listeners) {
    if (!listener->IsPrivilegedInterface()) {
      listener->socket()->proactor()->Await(
          [listener, maxclients]() { listener->SetMaxClients(maxclients); });
    }
  }
}

void SetSlowLogMaxLen(util::ProactorPool& pool, uint32_t val) {
  pool.AwaitFiberOnAll(
      [&val](auto index, auto* context) { ServerState::tlocal()->GetSlowLog().ChangeLength(val); });
}

void SetSlowLogThreshold(util::ProactorPool& pool, int32_t val) {
  pool.AwaitFiberOnAll([val](auto index, auto* context) {
    ServerState::tlocal()->log_slower_than_usec = val < 0 ? UINT32_MAX : uint32_t(val);
  });
}

void ServerFamily::Init(util::AcceptServer* acceptor, std::vector<facade::Listener*> listeners) {
  CHECK(acceptor_ == nullptr);
  acceptor_ = acceptor;
  listeners_ = std::move(listeners);
  dfly_cmd_ = make_unique<DflyCmd>(this);

  auto os_string = GetOSString();
  LOG_FIRST_N(INFO, 1) << "Host OS: " << os_string << " with " << shard_set->pool()->size()
                       << " threads";
  SetMaxClients(listeners_, absl::GetFlag(FLAGS_maxclients));
  config_registry.RegisterMutable("maxclients", [this](const absl::CommandLineFlag& flag) {
    auto res = flag.TryGet<uint32_t>();
    if (res.has_value())
      SetMaxClients(listeners_, res.value());
    return res.has_value();
  });

  SetSlowLogThreshold(service_.proactor_pool(), absl::GetFlag(FLAGS_slowlog_log_slower_than));
  config_registry.RegisterMutable("slowlog_log_slower_than",
                                  [this](const absl::CommandLineFlag& flag) {
                                    auto res = flag.TryGet<int32_t>();
                                    if (res.has_value())
                                      SetSlowLogThreshold(service_.proactor_pool(), res.value());
                                    return res.has_value();
                                  });
  SetSlowLogMaxLen(service_.proactor_pool(), absl::GetFlag(FLAGS_slowlog_max_len));
  config_registry.RegisterMutable("slowlog_max_len", [this](const absl::CommandLineFlag& flag) {
    auto res = flag.TryGet<uint32_t>();
    if (res.has_value())
      SetSlowLogMaxLen(service_.proactor_pool(), res.value());
    return res.has_value();
  });

  // We only reconfigure TLS when the 'tls' config key changes. Therefore to
  // update TLS certs, first update tls_cert_file, then set 'tls true'.
  config_registry.RegisterMutable("tls", [this](const absl::CommandLineFlag& flag) {
    if (!ValidateServerTlsFlags()) {
      return false;
    }
    for (facade::Listener* l : listeners_) {
      // Must reconfigure in the listener proactor to avoid a race.
      if (!l->socket()->proactor()->Await([l] { return l->ReconfigureTLS(); })) {
        return false;
      }
    }
    return true;
  });
  config_registry.RegisterMutable("tls_cert_file");
  config_registry.RegisterMutable("tls_key_file");
  config_registry.RegisterMutable("tls_ca_cert_file");
  config_registry.RegisterMutable("tls_ca_cert_dir");

  pb_task_ = shard_set->pool()->GetNextProactor();
  if (pb_task_->GetKind() == ProactorBase::EPOLL) {
    fq_threadpool_.reset(new FiberQueueThreadPool(absl::GetFlag(FLAGS_epoll_file_threads)));
  }

  string flag_dir = GetFlag(FLAGS_dir);
  if (IsCloudPath(flag_dir)) {
    shard_set->pool()->GetNextProactor()->Await([&] { util::aws::Init(); });
    snapshot_storage_ = std::make_shared<detail::AwsS3SnapshotStorage>(
        absl::GetFlag(FLAGS_s3_endpoint), absl::GetFlag(FLAGS_s3_use_https),
        absl::GetFlag(FLAGS_s3_ec2_metadata), absl::GetFlag(FLAGS_s3_sign_payload));
  } else if (fq_threadpool_) {
    snapshot_storage_ = std::make_shared<detail::FileSnapshotStorage>(fq_threadpool_.get());
  } else {
    snapshot_storage_ = std::make_shared<detail::FileSnapshotStorage>(nullptr);
  }

  // check for '--replicaof' before loading anything
  if (ReplicaOfFlag flag = GetFlag(FLAGS_replicaof); flag.has_value()) {
    service_.proactor_pool().GetNextProactor()->Await(
        [this, &flag]() { this->Replicate(flag.host, flag.port); });
  } else {  // load from snapshot only if --replicaof is empty
    LoadFromSnapshot();
  }

  const auto create_snapshot_schedule_fb = [this] {
    snapshot_schedule_fb_ =
        service_.proactor_pool().GetNextProactor()->LaunchFiber([this] { SnapshotScheduling(); });
  };
  config_registry.RegisterMutable(
      "snapshot_cron", [this, create_snapshot_schedule_fb](const absl::CommandLineFlag& flag) {
        JoinSnapshotSchedule();
        create_snapshot_schedule_fb();
        return true;
      });
  create_snapshot_schedule_fb();
}

void ServerFamily::LoadFromSnapshot() {
  const auto load_path_result =
      snapshot_storage_->LoadPath(GetFlag(FLAGS_dir), GetFlag(FLAGS_dbfilename));
  if (load_path_result) {
    const std::string load_path = *load_path_result;
    if (!load_path.empty()) {
      load_result_ = Load(load_path);
    }
  } else {
    if (std::error_code(load_path_result.error()) == std::errc::no_such_file_or_directory) {
      LOG(WARNING) << "Load snapshot: No snapshot found";
    } else {
      LOG(ERROR) << "Failed to load snapshot: " << load_path_result.error().Format();
    }
  }
}

void ServerFamily::JoinSnapshotSchedule() {
  schedule_done_.Notify();
  snapshot_schedule_fb_.JoinIfNeeded();
  schedule_done_.Reset();
}

void ServerFamily::Shutdown() {
  VLOG(1) << "ServerFamily::Shutdown";

  if (load_result_.valid())
    load_result_.wait();

  JoinSnapshotSchedule();

  if (save_on_shutdown_ && !absl::GetFlag(FLAGS_dbfilename).empty()) {
    shard_set->pool()->GetNextProactor()->Await([this] {
      if (GenericError ec = DoSave(); ec) {
        LOG(WARNING) << "Failed to perform snapshot " << ec.Format();
      }
    });
  }

  pb_task_->Await([this] {
    if (stats_caching_task_) {
      pb_task_->CancelPeriodic(stats_caching_task_);
      stats_caching_task_ = 0;
    }

    if (journal_->EnterLameDuck()) {
      auto ec = journal_->Close();
      LOG_IF(ERROR, ec) << "Error closing journal " << ec;
    }

    unique_lock lk(replicaof_mu_);
    if (replica_) {
      replica_->Stop();
    }

    dfly_cmd_->Shutdown();
    DebugCmd::Shutdown();
  });
}

struct AggregateLoadResult {
  AggregateError first_error;
  std::atomic<size_t> keys_read;
};

// Load starts as many fibers as there are files to load each one separately.
// It starts one more fiber that waits for all load fibers to finish and returns the first
// error (if any occured) with a future.
Future<GenericError> ServerFamily::Load(const std::string& load_path) {
  auto paths_result = snapshot_storage_->LoadPaths(load_path);
  if (!paths_result) {
    LOG(ERROR) << "Failed to load snapshot: " << paths_result.error().Format();

    Promise<GenericError> ec_promise;
    ec_promise.set_value(paths_result.error());
    return ec_promise.get_future();
  }

  std::vector<std::string> paths = *paths_result;

  LOG(INFO) << "Loading " << load_path;

  auto new_state = service_.SwitchState(GlobalState::ACTIVE, GlobalState::LOADING);
  if (new_state.first != GlobalState::LOADING) {
    LOG(WARNING) << GlobalStateName(new_state.first) << " in progress, ignored";
    return {};
  }

  RdbLoader::PerformPreLoad(&service_);

  auto& pool = service_.proactor_pool();

  vector<Fiber> load_fibers;
  load_fibers.reserve(paths.size());

  auto aggregated_result = std::make_shared<AggregateLoadResult>();

  for (auto& path : paths) {
    // For single file, choose thread that does not handle shards if possible.
    // This will balance out the CPU during the load.
    ProactorBase* proactor;
    if (paths.size() == 1 && shard_count() < pool.size()) {
      proactor = pool.at(shard_count());
    } else {
      proactor = pool.GetNextProactor();
    }

    auto load_fiber = [this, aggregated_result, path = std::move(path)]() {
      auto load_result = LoadRdb(path);
      if (load_result.has_value())
        aggregated_result->keys_read.fetch_add(*load_result);
      else
        aggregated_result->first_error = load_result.error();
    };
    load_fibers.push_back(proactor->LaunchFiber(std::move(load_fiber)));
  }

  Promise<GenericError> ec_promise;
  Future<GenericError> ec_future = ec_promise.get_future();

  // Run fiber that empties the channel and sets ec_promise.
  auto load_join_fiber = [this, aggregated_result, load_fibers = std::move(load_fibers),
                          ec_promise = std::move(ec_promise)]() mutable {
    for (auto& fiber : load_fibers) {
      fiber.Join();
    }

    if (aggregated_result->first_error) {
      LOG(ERROR) << "Rdb load failed. " << (*aggregated_result->first_error).message();
      exit(1);
    }

    RdbLoader::PerformPostLoad(&service_);

    LOG(INFO) << "Load finished, num keys read: " << aggregated_result->keys_read;
    service_.SwitchState(GlobalState::LOADING, GlobalState::ACTIVE);
    ec_promise.set_value(*(aggregated_result->first_error));
  };
  pool.GetNextProactor()->Dispatch(std::move(load_join_fiber));

  return ec_future;
}

void ServerFamily::SnapshotScheduling() {
  const std::optional<cron::cronexpr> cron_expr = InferSnapshotCronExpr();
  if (!cron_expr) {
    return;
  }

  const auto loading_check_interval = std::chrono::seconds(10);
  while (service_.GetGlobalState() == GlobalState::LOADING) {
    schedule_done_.WaitFor(loading_check_interval);
  }

  while (true) {
    const std::chrono::time_point now = std::chrono::system_clock::now();
    const std::chrono::time_point next = cron::cron_next(cron_expr.value(), now);

    if (schedule_done_.WaitFor(next - now)) {
      break;
    };

    GenericError ec = DoSave();
    if (ec) {
      LOG(WARNING) << "Failed to perform snapshot " << ec.Format();
    }
  }
}

io::Result<size_t> ServerFamily::LoadRdb(const std::string& rdb_file) {
  error_code ec;
  io::ReadonlyFileOrError res = snapshot_storage_->OpenReadFile(rdb_file);
  if (res) {
    io::FileSource fs(*res);

    RdbLoader loader{&service_};
    ec = loader.Load(&fs);
    if (!ec) {
      VLOG(1) << "Done loading RDB from " << rdb_file << ", keys loaded: " << loader.keys_loaded();
      VLOG(1) << "Loading finished after " << strings::HumanReadableElapsedTime(loader.load_time());
      return loader.keys_loaded();
    }
  } else {
    ec = res.error();
  }
  return nonstd::make_unexpected(ec);
}

enum MetricType { COUNTER, GAUGE, SUMMARY, HISTOGRAM };

const char* MetricTypeName(MetricType type) {
  switch (type) {
    case MetricType::COUNTER:
      return "counter";
    case MetricType::GAUGE:
      return "gauge";
    case MetricType::SUMMARY:
      return "summary";
    case MetricType::HISTOGRAM:
      return "histogram";
  }
  return "unknown";
}

inline string GetMetricFullName(string_view metric_name) {
  return StrCat("dragonfly_", metric_name);
}

void AppendMetricHeader(string_view metric_name, string_view metric_help, MetricType type,
                        string* dest) {
  const auto full_metric_name = GetMetricFullName(metric_name);
  absl::StrAppend(dest, "# HELP ", full_metric_name, " ", metric_help, "\n");
  absl::StrAppend(dest, "# TYPE ", full_metric_name, " ", MetricTypeName(type), "\n");
}

void AppendLabelTupple(absl::Span<const string_view> label_names,
                       absl::Span<const string_view> label_values, string* dest) {
  if (label_names.empty())
    return;

  absl::StrAppend(dest, "{");
  for (size_t i = 0; i < label_names.size(); ++i) {
    if (i > 0) {
      absl::StrAppend(dest, ", ");
    }
    absl::StrAppend(dest, label_names[i], "=\"", label_values[i], "\"");
  }

  absl::StrAppend(dest, "}");
}

void AppendMetricValue(string_view metric_name, const absl::AlphaNum& value,
                       absl::Span<const string_view> label_names,
                       absl::Span<const string_view> label_values, string* dest) {
  absl::StrAppend(dest, GetMetricFullName(metric_name));
  AppendLabelTupple(label_names, label_values, dest);
  absl::StrAppend(dest, " ", value, "\n");
}

void AppendMetricWithoutLabels(string_view name, string_view help, const absl::AlphaNum& value,
                               MetricType type, string* dest) {
  AppendMetricHeader(name, help, type, dest);
  AppendMetricValue(name, value, {}, {}, dest);
}

void PrintPrometheusMetrics(const Metrics& m, StringResponse* resp) {
  // Server metrics
  AppendMetricHeader("version", "", MetricType::GAUGE, &resp->body());
  AppendMetricValue("version", 1, {"version"}, {GetVersion()}, &resp->body());

  bool is_master = ServerState::tlocal()->is_master;
  AppendMetricWithoutLabels("master", "1 if master 0 if replica", is_master ? 1 : 0,
                            MetricType::GAUGE, &resp->body());
  AppendMetricWithoutLabels("uptime_in_seconds", "", m.uptime, MetricType::COUNTER, &resp->body());

  // Clients metrics
  const auto& conn_stats = m.facade_stats.conn_stats;
  AppendMetricWithoutLabels("connected_clients", "", conn_stats.num_conns, MetricType::GAUGE,
                            &resp->body());
  AppendMetricWithoutLabels("client_read_buffer_bytes", "", conn_stats.read_buf_capacity,
                            MetricType::GAUGE, &resp->body());
  AppendMetricWithoutLabels("blocked_clients", "", conn_stats.num_blocked_clients,
                            MetricType::GAUGE, &resp->body());
  AppendMetricWithoutLabels("dispatch_queue_bytes", "", conn_stats.dispatch_queue_bytes,
                            MetricType::GAUGE, &resp->body());

  // Memory metrics
  auto sdata_res = io::ReadStatusInfo();
  AppendMetricWithoutLabels("memory_used_bytes", "", m.heap_used_bytes, MetricType::GAUGE,
                            &resp->body());
  AppendMetricWithoutLabels("memory_used_peak_bytes", "", used_mem_peak.load(memory_order_relaxed),
                            MetricType::GAUGE, &resp->body());
  AppendMetricWithoutLabels("comitted_memory", "", GetMallocCurrentCommitted(), MetricType::GAUGE,
                            &resp->body());
  AppendMetricWithoutLabels("memory_max_bytes", "", max_memory_limit, MetricType::GAUGE,
                            &resp->body());
  if (sdata_res.has_value()) {
    size_t rss = sdata_res->vm_rss + sdata_res->hugetlb_pages;
    AppendMetricWithoutLabels("used_memory_rss_bytes", "", rss, MetricType::GAUGE, &resp->body());
  } else {
    LOG_FIRST_N(ERROR, 10) << "Error fetching /proc/self/status stats. error "
                           << sdata_res.error().message();
  }

  DbStats total;
  for (const auto& db_stats : m.db_stats) {
    total += db_stats;
  }

  {
    string type_used_memory_metric;
    AppendMetricHeader("type_used_memory", "Memory used per type", MetricType::GAUGE,
                       &type_used_memory_metric);
    for (unsigned type = 0; type < total.memory_usage_by_type.size(); type++) {
      size_t mem = total.memory_usage_by_type[type];
      if (mem > 0) {
        AppendMetricValue("type_used_memory", mem, {"type"}, {CompactObj::ObjTypeToString(type)},
                          &type_used_memory_metric);
      }
    }
    absl::StrAppend(&resp->body(), type_used_memory_metric);
  }

  // Stats metrics
  AppendMetricWithoutLabels("connections_received_total", "", conn_stats.conn_received_cnt,
                            MetricType::COUNTER, &resp->body());

  AppendMetricWithoutLabels("commands_processed_total", "", conn_stats.command_cnt,
                            MetricType::COUNTER, &resp->body());
  AppendMetricWithoutLabels("keyspace_hits_total", "", m.events.hits, MetricType::COUNTER,
                            &resp->body());
  AppendMetricWithoutLabels("keyspace_misses_total", "", m.events.misses, MetricType::COUNTER,
                            &resp->body());
  AppendMetricWithoutLabels("keyspace_mutations_total", "", m.events.mutations, MetricType::COUNTER,
                            &resp->body());

  // Net metrics
  AppendMetricWithoutLabels("net_input_bytes_total", "", conn_stats.io_read_bytes,
                            MetricType::COUNTER, &resp->body());
  AppendMetricWithoutLabels("net_output_bytes_total", "", m.facade_stats.reply_stats.io_write_bytes,
                            MetricType::COUNTER, &resp->body());
  {
    string send_latency_metrics;
    constexpr string_view kReplyLatency = "reply_duration_seconds";
    AppendMetricHeader(kReplyLatency, "Reply latency per type", MetricType::COUNTER,
                       &send_latency_metrics);

    string send_count_metrics;
    constexpr string_view kReplyCount = "reply_total";
    AppendMetricHeader(kReplyCount, "Reply count per type", MetricType::COUNTER,
                       &send_count_metrics);

    for (unsigned i = 0; i < SendStatsType::kNumTypes; ++i) {
      auto& stats = m.facade_stats.reply_stats.send_stats[i];
      string_view type;
      switch (SendStatsType(i)) {
        case SendStatsType::kRegular:
          type = "regular";
          break;
        case SendStatsType::kBatch:
          type = "batch";
          break;
        case SendStatsType::kNumTypes:
          type = "other";
          break;
      }

      AppendMetricValue(kReplyLatency, stats.total_duration / 1'000'000, {"type"}, {type},
                        &send_latency_metrics);
      AppendMetricValue(kReplyCount, stats.count, {"type"}, {type}, &send_count_metrics);
    }

    absl::StrAppend(&resp->body(), send_latency_metrics);
    absl::StrAppend(&resp->body(), send_count_metrics);
  }

  // DB stats
  AppendMetricWithoutLabels("expired_keys_total", "", m.events.expired_keys, MetricType::COUNTER,
                            &resp->body());
  AppendMetricWithoutLabels("evicted_keys_total", "", m.events.evicted_keys, MetricType::COUNTER,
                            &resp->body());

  string db_key_metrics;
  string db_key_expire_metrics;

  AppendMetricHeader("db_keys", "Total number of keys by DB", MetricType::GAUGE, &db_key_metrics);
  AppendMetricHeader("db_keys_expiring", "Total number of expiring keys by DB", MetricType::GAUGE,
                     &db_key_expire_metrics);

  for (size_t i = 0; i < m.db_stats.size(); ++i) {
    AppendMetricValue("db_keys", m.db_stats[i].key_count, {"db"}, {StrCat("db", i)},
                      &db_key_metrics);
    AppendMetricValue("db_keys_expiring", m.db_stats[i].expire_count, {"db"}, {StrCat("db", i)},
                      &db_key_expire_metrics);
  }

  // Command stats
  {
    string command_metrics;

    AppendMetricHeader("commands", "Metrics for all commands ran", MetricType::COUNTER,
                       &command_metrics);
    for (const auto& [name, stat] : m.cmd_stats_map) {
      const auto calls = stat.first;
      const double duration_seconds = stat.second * 1e-6;
      AppendMetricValue("commands_total", calls, {"cmd"}, {name}, &command_metrics);
      AppendMetricValue("commands_duration_seconds", duration_seconds, {"cmd"}, {name},
                        &command_metrics);
    }
    absl::StrAppend(&resp->body(), command_metrics);
  }

  if (!m.replication_metrics.empty()) {
    string replication_lag_metrics;
    AppendMetricHeader("connected_replica_lag_records", "Lag in records of a connected replica.",
                       MetricType::GAUGE, &replication_lag_metrics);
    for (const auto& replica : m.replication_metrics) {
      AppendMetricValue("connected_replica_lag_records", replica.lsn_lag,
                        {"replica_ip", "replica_port", "replica_state"},
                        {replica.address, absl::StrCat(replica.listening_port), replica.state},
                        &replication_lag_metrics);
    }
    absl::StrAppend(&resp->body(), replication_lag_metrics);
  }

  AppendMetricWithoutLabels("fiber_switch_total", "", m.fiber_switch_cnt, MetricType::COUNTER,
                            &resp->body());
  double delay_seconds = m.fiber_switch_delay_usec * 1e-6;
  AppendMetricWithoutLabels("fiber_switch_delay_seconds_total", "", delay_seconds,
                            MetricType::COUNTER, &resp->body());

  AppendMetricWithoutLabels("fiber_longrun_total", "", m.fiber_longrun_cnt, MetricType::COUNTER,
                            &resp->body());
  double longrun_seconds = m.fiber_longrun_usec * 1e-6;
  AppendMetricWithoutLabels("fiber_longrun_seconds_total", "", longrun_seconds, MetricType::COUNTER,
                            &resp->body());
  AppendMetricWithoutLabels("tx_queue_len", "", m.tx_queue_len, MetricType::GAUGE, &resp->body());

  AppendMetricHeader("transaction_widths_total", "Transaction counts by their widths",
                     MetricType::COUNTER, &resp->body());

  for (unsigned width = 0; width < shard_set->size(); ++width) {
    uint64_t count = m.coordinator_stats.tx_width_freq_arr[width];

    if (count > 0) {
      AppendMetricValue("transaction_widths_total", count, {"width"}, {StrCat("w", width + 1)},
                        &resp->body());
    }
  }

  const auto& tc = m.coordinator_stats.tx_type_cnt;
  AppendMetricHeader("transaction_types_total", "Transaction counts by their types",
                     MetricType::COUNTER, &resp->body());

  const char* kTxTypeNames[ServerState::NUM_TX_TYPES] = {"global", "normal", "ooo", "quick",
                                                         "inline"};
  for (unsigned type = 0; type < ServerState::NUM_TX_TYPES; ++type) {
    if (tc[type] > 0) {
      AppendMetricValue("transaction_types_total", tc[type], {"type"}, {kTxTypeNames[type]},
                        &resp->body());
    }
  }

  absl::StrAppend(&resp->body(), db_key_metrics);
  absl::StrAppend(&resp->body(), db_key_expire_metrics);
}

void ServerFamily::ConfigureMetrics(util::HttpListenerBase* http_base) {
  // The naming of the metrics should be compatible with redis_exporter, see
  // https://github.com/oliver006/redis_exporter/blob/master/exporter/exporter.go#L111

  auto cb = [this](const util::http::QueryArgs& args, util::HttpContext* send) {
    StringResponse resp = util::http::MakeStringResponse(boost::beast::http::status::ok);
    PrintPrometheusMetrics(this->GetMetrics(), &resp);

    return send->Invoke(std::move(resp));
  };

  http_base->RegisterCb("/metrics", cb);
}

void ServerFamily::PauseReplication(bool pause) {
  unique_lock lk(replicaof_mu_);

  // Switch to primary mode.
  if (!ServerState::tlocal()->is_master) {
    auto repl_ptr = replica_;
    CHECK(repl_ptr);
    repl_ptr->Pause(pause);
  }
}

std::optional<ReplicaOffsetInfo> ServerFamily::GetReplicaOffsetInfo() {
  unique_lock lk(replicaof_mu_);

  // Switch to primary mode.
  if (!ServerState::tlocal()->is_master) {
    auto repl_ptr = replica_;
    CHECK(repl_ptr);
    return ReplicaOffsetInfo{repl_ptr->GetSyncId(), repl_ptr->GetReplicaOffset()};
  }
  return nullopt;
}

bool ServerFamily::HasReplica() const {
  unique_lock lk(replicaof_mu_);
  return replica_ != nullptr;
}

optional<Replica::Info> ServerFamily::GetReplicaInfo() const {
  unique_lock lk(replicaof_mu_);
  if (replica_ == nullptr) {
    return nullopt;
  } else {
    return replica_->GetInfo();
  }
}

string ServerFamily::GetReplicaMasterId() const {
  unique_lock lk(replicaof_mu_);
  return string(replica_->MasterId());
}

void ServerFamily::OnClose(ConnectionContext* cntx) {
  dfly_cmd_->OnClose(cntx);
}

void ServerFamily::StatsMC(std::string_view section, facade::ConnectionContext* cntx) {
  if (!section.empty()) {
    return cntx->reply_builder()->SendError("");
  }
  string info;

#define ADD_LINE(name, val) absl::StrAppend(&info, "STAT " #name " ", val, "\r\n")

  time_t now = time(NULL);
  struct rusage ru;
  getrusage(RUSAGE_SELF, &ru);

  auto dbl_time = [](const timeval& tv) -> double {
    return tv.tv_sec + double(tv.tv_usec) / 1000000.0;
  };

  double utime = dbl_time(ru.ru_utime);
  double systime = dbl_time(ru.ru_stime);

  Metrics m = GetMetrics();

  ADD_LINE(pid, getpid());
  ADD_LINE(uptime, m.uptime);
  ADD_LINE(time, now);
  ADD_LINE(version, kGitTag);
  ADD_LINE(libevent, "iouring");
  ADD_LINE(pointer_size, sizeof(void*));
  ADD_LINE(rusage_user, utime);
  ADD_LINE(rusage_system, systime);
  ADD_LINE(max_connections, -1);
  ADD_LINE(curr_connections, m.facade_stats.conn_stats.num_conns);
  ADD_LINE(total_connections, -1);
  ADD_LINE(rejected_connections, -1);
  ADD_LINE(bytes_read, m.facade_stats.conn_stats.io_read_bytes);
  ADD_LINE(bytes_written, m.facade_stats.reply_stats.io_write_bytes);
  ADD_LINE(limit_maxbytes, -1);

  absl::StrAppend(&info, "END\r\n");

  MCReplyBuilder* builder = static_cast<MCReplyBuilder*>(cntx->reply_builder());
  builder->SendRaw(info);

#undef ADD_LINE
}

GenericError ServerFamily::DoSave(bool ignore_state) {
  const CommandId* cid = service().FindCmd("SAVE");
  CHECK_NOTNULL(cid);
  boost::intrusive_ptr<Transaction> trans(new Transaction{cid});
  trans->InitByArgs(0, {});
  return DoSave(absl::GetFlag(FLAGS_df_snapshot_format), {}, trans.get(), ignore_state);
}

GenericError ServerFamily::DoSave(bool new_version, string_view basename, Transaction* trans,
                                  bool ignore_state) {
  if (!ignore_state) {
    auto [new_state, success] = service_.SwitchState(GlobalState::ACTIVE, GlobalState::SAVING);
    if (!success) {
      return GenericError{make_error_code(errc::operation_in_progress),
                          StrCat(GlobalStateName(new_state), " - can not save database")};
    }
  }
  {
    std::lock_guard lck(save_mu_);
    start_save_time_ = absl::Now();
  }
  SaveStagesController sc{detail::SaveStagesInputs{
      new_version, basename, trans, &service_, &is_saving_, fq_threadpool_.get(), &last_save_info_,
      &save_mu_, &save_bytes_cb_, snapshot_storage_}};
  auto res = sc.Save();
  {
    std::lock_guard lck(save_mu_);
    start_save_time_.reset();
  }
  if (!ignore_state)
    service_.SwitchState(GlobalState::SAVING, GlobalState::ACTIVE);
  return res;
}

error_code ServerFamily::Drakarys(Transaction* transaction, DbIndex db_ind) {
  VLOG(1) << "Drakarys";

  transaction->Schedule();  // TODO: to convert to ScheduleSingleHop ?

  transaction->Execute(
      [db_ind](Transaction* t, EngineShard* shard) {
        shard->db_slice().FlushDb(db_ind);
        return OpStatus::OK;
      },
      true);

  return error_code{};
}

LastSaveInfo ServerFamily::GetLastSaveInfo() const {
  lock_guard lk(save_mu_);
  return last_save_info_;
}

void ServerFamily::DbSize(CmdArgList args, ConnectionContext* cntx) {
  atomic_ulong num_keys{0};

  shard_set->RunBriefInParallel(
      [&](EngineShard* shard) {
        auto db_size = shard->db_slice().DbSize(cntx->conn_state.db_index);
        num_keys.fetch_add(db_size, memory_order_relaxed);
      },
      [](ShardId) { return true; });

  return cntx->SendLong(num_keys.load(memory_order_relaxed));
}

void ServerFamily::BreakOnShutdown() {
  dfly_cmd_->BreakOnShutdown();
}

void ServerFamily::CancelBlockingOnThread(std::function<OpStatus(ArgSlice)> status_cb) {
  auto cb = [status_cb](unsigned thread_index, util::Connection* conn) {
    if (auto fcntx = static_cast<facade::Connection*>(conn)->cntx(); fcntx) {
      auto* cntx = static_cast<ConnectionContext*>(fcntx);
      if (cntx->transaction && cntx->blocked) {
        cntx->transaction->CancelBlocking(status_cb);
      }
    }
  };

  for (auto* listener : listeners_)
    listener->TraverseConnectionsOnThread(cb);
}

string GetPassword() {
  string flag = GetFlag(FLAGS_requirepass);
  if (!flag.empty()) {
    return flag;
  }

  const char* env_var = getenv("DFLY_PASSWORD");
  if (env_var) {
    return env_var;
  }

  return "";
}

void ServerFamily::SendInvalidationMessages() const {
  // send invalidation message (caused by flushdb) to all the clients which
  // turned on client tracking
  auto cb = [](unsigned thread_index, util::Connection* conn) {
    facade::ConnectionContext* fc = static_cast<facade::Connection*>(conn)->cntx();
    if (fc) {
      ConnectionContext* cntx = static_cast<ConnectionContext*>(fc);
      if (cntx->conn()->IsTrackingOn()) {
        facade::Connection::InvalidationMessage x;
        x.invalidate_due_to_flush = true;
        cntx->conn()->SendInvalidationMessageAsync(x);
      }
    }
  };
  for (auto* listener : listeners_) {
    listener->TraverseConnections(cb);
  }
}

void ServerFamily::FlushDb(CmdArgList args, ConnectionContext* cntx) {
  DCHECK(cntx->transaction);
  Drakarys(cntx->transaction, cntx->transaction->GetDbIndex());
  SendInvalidationMessages();
  cntx->reply_builder()->SendOk();
}

void ServerFamily::FlushAll(CmdArgList args, ConnectionContext* cntx) {
  if (args.size() > 1) {
    cntx->SendError(kSyntaxErr);
    return;
  }

  DCHECK(cntx->transaction);
  Drakarys(cntx->transaction, DbSlice::kDbAll);
  SendInvalidationMessages();
  cntx->SendOk();
}

void ServerFamily::Auth(CmdArgList args, ConnectionContext* cntx) {
  if (args.size() > 2) {
    return cntx->SendError(kSyntaxErr);
  }

  // non admin port auth
  if (!cntx->conn()->IsPrivileged()) {
    const auto* registry = ServerState::tlocal()->user_registry;
    const bool one_arg = args.size() == 1;
    std::string_view username = one_arg ? "default" : facade::ToSV(args[0]);
    const size_t index = one_arg ? 0 : 1;
    std::string_view password = facade::ToSV(args[index]);
    auto is_authorized = registry->AuthUser(username, password);
    if (is_authorized) {
      cntx->authed_username = username;
      auto cred = registry->GetCredentials(username);
      cntx->acl_categories = cred.acl_categories;
      cntx->acl_commands = cred.acl_commands;
      cntx->keys = std::move(cred.keys);
      cntx->authenticated = true;
      return cntx->SendOk();
    }
    auto& log = ServerState::tlocal()->acl_log;
    using Reason = acl::AclLog::Reason;
    log.Add(*cntx, "AUTH", Reason::AUTH, std::string(username));
    return cntx->SendError(facade::kAuthRejected);
  }

  if (!cntx->req_auth) {
    return cntx->SendError(
        "AUTH <password> called without any password configured for "
        "admin port. Are you sure your configuration is correct?");
  }

  string_view pass = ArgS(args, 0);
  if (pass == GetPassword()) {
    cntx->authenticated = true;
    cntx->SendOk();
  } else {
    cntx->SendError(facade::kAuthRejected);
  }
}

void ServerFamily::Client(CmdArgList args, ConnectionContext* cntx) {
  ToUpper(&args[0]);
  string_view sub_cmd = ArgS(args, 0);
  CmdArgList sub_args = args.subspan(1);

  if (sub_cmd == "SETNAME") {
    return ClientSetName(sub_args, cntx);
  } else if (sub_cmd == "GETNAME") {
    return ClientGetName(sub_args, cntx);
  } else if (sub_cmd == "LIST") {
    return ClientList(sub_args, absl::MakeSpan(listeners_), cntx);
  } else if (sub_cmd == "PAUSE") {
    return ClientPauseCmd(sub_args, absl::MakeSpan(listeners_), cntx);
  } else if (sub_cmd == "TRACKING") {
    return ClientTracking(sub_args, cntx);
  } else if (sub_cmd == "KILL") {
    return ClientKill(sub_args, absl::MakeSpan(listeners_), cntx);
  }

  if (sub_cmd == "SETINFO") {
    return cntx->SendOk();
  }

  LOG_FIRST_N(ERROR, 10) << "Subcommand " << sub_cmd << " not supported";
  return cntx->SendError(UnknownSubCmd(sub_cmd, "CLIENT"), kSyntaxErrType);
}

void ServerFamily::Config(CmdArgList args, ConnectionContext* cntx) {
  ToUpper(&args[0]);
  string_view sub_cmd = ArgS(args, 0);

  if (sub_cmd == "SET") {
    if (args.size() != 3) {
      return cntx->SendError(WrongNumArgsError("config|set"));
    }

    ToLower(&args[1]);
    string_view param = ArgS(args, 1);

    ConfigRegistry::SetResult result = config_registry.Set(param, ArgS(args, 2));

    const char kErrPrefix[] = "CONFIG SET failed (possibly related to argument '";
    switch (result) {
      case ConfigRegistry::SetResult::OK:
        return cntx->SendOk();
      case ConfigRegistry::SetResult::UNKNOWN:
        return cntx->SendError(
            absl::StrCat("Unknown option or number of arguments for CONFIG SET - '", param, "'"),
            kConfigErrType);

      case ConfigRegistry::SetResult::READONLY:
        return cntx->SendError(absl::StrCat(kErrPrefix, param, "') - can't set immutable config"),
                               kConfigErrType);

      case ConfigRegistry::SetResult::INVALID:
        return cntx->SendError(absl::StrCat(kErrPrefix, param, "') - argument can not be set"),
                               kConfigErrType);
    }
    ABSL_UNREACHABLE();
  }

  if (sub_cmd == "GET" && args.size() == 2) {
    vector<string> res;
    string_view param = ArgS(args, 1);

    // Support 'databases' for backward compatibility.
    if (param == "databases") {
      res.emplace_back(param);
      res.push_back(absl::StrCat(absl::GetFlag(FLAGS_dbnum)));
    } else {
      vector<string> names = config_registry.List(param);

      for (const auto& name : names) {
        absl::CommandLineFlag* flag = CHECK_NOTNULL(absl::FindCommandLineFlag(name));
        res.push_back(name);
        res.push_back(flag->CurrentValue());
      }
    }
    auto* rb = static_cast<RedisReplyBuilder*>(cntx->reply_builder());
    return rb->SendStringArr(res, RedisReplyBuilder::MAP);
  }

  if (sub_cmd == "RESETSTAT") {
    ResetStat();
    return cntx->SendOk();
  } else {
    return cntx->SendError(UnknownSubCmd(sub_cmd, "CONFIG"), kSyntaxErrType);
  }
}

void ServerFamily::Debug(CmdArgList args, ConnectionContext* cntx) {
  ToUpper(&args[0]);

  DebugCmd dbg_cmd{this, cntx};

  return dbg_cmd.Run(args);
}

void ServerFamily::Memory(CmdArgList args, ConnectionContext* cntx) {
  ToUpper(&args[0]);

  MemoryCmd mem_cmd{this, cntx};

  return mem_cmd.Run(args);
}

// SAVE [DF|RDB] [basename]
// Allows saving the snapshot of the dataset on disk, potentially overriding the format
// and the snapshot name.
void ServerFamily::Save(CmdArgList args, ConnectionContext* cntx) {
  string err_detail;
  bool new_version = absl::GetFlag(FLAGS_df_snapshot_format);
  if (args.size() > 2) {
    return cntx->SendError(kSyntaxErr);
  }

  if (args.size() >= 1) {
    ToUpper(&args[0]);
    string_view sub_cmd = ArgS(args, 0);
    if (sub_cmd == "DF") {
      new_version = true;
    } else if (sub_cmd == "RDB") {
      new_version = false;
    } else {
      return cntx->SendError(UnknownSubCmd(sub_cmd, "SAVE"), kSyntaxErrType);
    }
  }

  string_view basename;
  if (args.size() == 2) {
    basename = ArgS(args, 1);
  }

  GenericError ec = DoSave(new_version, basename, cntx->transaction);
  if (ec) {
    cntx->SendError(ec.Format());
  } else {
    cntx->SendOk();
  }
}

static void MergeDbSliceStats(const DbSlice::Stats& src, Metrics* dest) {
  if (src.db_stats.size() > dest->db_stats.size())
    dest->db_stats.resize(src.db_stats.size());

  for (size_t i = 0; i < src.db_stats.size(); ++i)
    dest->db_stats[i] += src.db_stats[i];

  dest->events += src.events;
  dest->small_string_bytes += src.small_string_bytes;
}

void ServerFamily::ResetStat() {
  shard_set->pool()->Await([registry = service_.mutable_registry(), this](unsigned index, auto*) {
    registry->ResetCallStats(index);
    SinkReplyBuilder::ResetThreadLocalStats();
    auto& stats = tl_facade_stats->conn_stats;
    stats.command_cnt = 0;
    stats.pipelined_cmd_cnt = 0;

    EngineShard* shard = EngineShard::tlocal();
    shard->db_slice().ResetEvents();
    tl_facade_stats->conn_stats.conn_received_cnt = 0;
    tl_facade_stats->conn_stats.pipelined_cmd_cnt = 0;
    tl_facade_stats->conn_stats.command_cnt = 0;
    tl_facade_stats->conn_stats.io_read_cnt = 0;
    tl_facade_stats->conn_stats.io_read_bytes = 0;

    tl_facade_stats->reply_stats.io_write_bytes = 0;
    tl_facade_stats->reply_stats.io_write_cnt = 0;
    for (auto& send_stat : tl_facade_stats->reply_stats.send_stats) {
      send_stat = {};
    }

    service_.mutable_registry()->ResetCallStats(index);
  });
}

Metrics ServerFamily::GetMetrics() const {
  Metrics result;
  Mutex mu;

  auto cmd_stat_cb = [&dest = result.cmd_stats_map](string_view name, const CmdCallStats& stat) {
    auto& [calls, sum] = dest[string{name}];
    calls += stat.first;
    sum += stat.second;
  };

  auto cb = [&](unsigned index, ProactorBase* pb) {
    EngineShard* shard = EngineShard::tlocal();
    ServerState* ss = ServerState::tlocal();

    lock_guard lk(mu);

    result.fiber_switch_cnt += fb2::FiberSwitchEpoch();
    result.fiber_switch_delay_usec += fb2::FiberSwitchDelayUsec();
    result.fiber_longrun_cnt += fb2::FiberLongRunCnt();
    result.fiber_longrun_usec += fb2::FiberLongRunSumUsec();

    result.coordinator_stats.Add(shard_set->size(), ss->stats);

    result.uptime = time(NULL) - this->start_time_;
    result.qps += uint64_t(ss->MovingSum6());
    result.facade_stats += *tl_facade_stats;

    if (shard) {
      result.heap_used_bytes += shard->UsedMemory();
      MergeDbSliceStats(shard->db_slice().GetStats(), &result);
      result.shard_stats += shard->stats();

      if (shard->tiered_storage())
        result.tiered_stats += shard->tiered_storage()->GetStats();
      if (shard->search_indices())
        result.search_stats += shard->search_indices()->GetStats();

      result.traverse_ttl_per_sec += shard->GetMovingSum6(EngineShard::TTL_TRAVERSE);
      result.delete_ttl_per_sec += shard->GetMovingSum6(EngineShard::TTL_DELETE);
      if (result.tx_queue_len < shard->txq()->size())
        result.tx_queue_len = shard->txq()->size();
    }

    service_.mutable_registry()->MergeCallStats(index, cmd_stat_cb);
  };

  service_.proactor_pool().AwaitFiberOnAll(std::move(cb));

  // Normalize moving average stats
  result.qps /= 6;
  result.traverse_ttl_per_sec /= 6;
  result.delete_ttl_per_sec /= 6;

  bool is_master = ServerState::tlocal() && ServerState::tlocal()->is_master;
  if (is_master)
    result.replication_metrics = dfly_cmd_->GetReplicasRoleInfo();

  // Update peak stats. We rely on the fact that GetMetrics is called frequently enough to
  // update peak_stats_ from it.
  lock_guard lk{peak_stats_mu_};
  UpdateMax(&peak_stats_.conn_dispatch_queue_bytes,
            result.facade_stats.conn_stats.dispatch_queue_bytes);
  UpdateMax(&peak_stats_.conn_read_buf_capacity, result.facade_stats.conn_stats.read_buf_capacity);

  result.peak_stats = peak_stats_;

  return result;
}

void ServerFamily::Info(CmdArgList args, ConnectionContext* cntx) {
  if (args.size() > 1) {
    return cntx->SendError(kSyntaxErr);
  }

  string_view section;

  if (args.size() == 1) {
    ToUpper(&args[0]);
    section = ArgS(args, 0);
  }

  string info;

  auto should_enter = [&](string_view name, bool hidden = false) {
    if ((!hidden && section.empty()) || section == "ALL" || section == name) {
      auto normalized_name = string{name.substr(0, 1)} + absl::AsciiStrToLower(name.substr(1));
      absl::StrAppend(&info, info.empty() ? "" : "\r\n", "# ", normalized_name, "\r\n");
      return true;
    }
    return false;
  };

  auto append = [&info](absl::AlphaNum a1, absl::AlphaNum a2) {
    absl::StrAppend(&info, a1, ":", a2, "\r\n");
  };

  Metrics m = GetMetrics();
  DbStats total;
  for (const auto& db_stats : m.db_stats)
    total += db_stats;

  if (should_enter("SERVER")) {
    auto kind = ProactorBase::me()->GetKind();
    const char* multiplex_api = (kind == ProactorBase::IOURING) ? "iouring" : "epoll";

    append("redis_version", kRedisVersion);
    append("dragonfly_version", GetVersion());
    append("redis_mode", GetRedisMode());
    append("arch_bits", 64);
    append("os", GetOSString());
    append("multiplexing_api", multiplex_api);
    append("tcp_port", GetFlag(FLAGS_port));
    append("thread_count", service_.proactor_pool().size());
    size_t uptime = m.uptime;
    append("uptime_in_seconds", uptime);
    append("uptime_in_days", uptime / (3600 * 24));
  }

  if (should_enter("CLIENTS")) {
    append("connected_clients", m.facade_stats.conn_stats.num_conns);
    append("client_read_buffer_bytes", m.facade_stats.conn_stats.read_buf_capacity);
    append("blocked_clients", m.facade_stats.conn_stats.num_blocked_clients);
    append("dispatch_queue_entries", m.facade_stats.conn_stats.dispatch_queue_entries);
  }

  if (should_enter("MEMORY")) {
    append("used_memory", m.heap_used_bytes);
    append("used_memory_human", HumanReadableNumBytes(m.heap_used_bytes));
    const auto ump = used_mem_peak.load(memory_order_relaxed);
    append("used_memory_peak", ump);
    append("used_memory_peak_human", HumanReadableNumBytes(ump));

    size_t rss = rss_mem_current.load(memory_order_relaxed);
    append("used_memory_rss", rss);
    append("used_memory_rss_human", HumanReadableNumBytes(rss));
    append("used_memory_peak_rss", rss_mem_peak.load(memory_order_relaxed));

    append("comitted_memory", GetMallocCurrentCommitted());

    append("maxmemory", max_memory_limit);
    append("maxmemory_human", HumanReadableNumBytes(max_memory_limit));

    // Blob - all these cases where the key/objects are represented by a single blob allocated on
    // heap. For example, strings or intsets. members of lists, sets, zsets etc
    // are not accounted for to avoid complex computations. In some cases, when number of members
    // is known we approximate their allocations by taking 16 bytes per member.
    append("object_used_memory", total.obj_memory_usage);

    for (unsigned type = 0; type < total.memory_usage_by_type.size(); type++) {
      size_t mem = total.memory_usage_by_type[type];
      if (mem > 0) {
        append(absl::StrCat("type_used_memory_", CompactObj::ObjTypeToString(type)), mem);
      }
    }
    append("table_used_memory", total.table_mem_usage);
    append("num_buckets", total.bucket_count);
    append("num_entries", total.key_count);
    append("inline_keys", total.inline_keys);
    append("listpack_blobs", total.listpack_blob_cnt);
    append("listpack_bytes", total.listpack_bytes);
    append("small_string_bytes", m.small_string_bytes);
    append("pipeline_cache_bytes", m.facade_stats.conn_stats.pipeline_cmd_cache_bytes);
    append("dispatch_queue_bytes", m.facade_stats.conn_stats.dispatch_queue_bytes);
    append("dispatch_queue_subscriber_bytes",
           m.facade_stats.conn_stats.dispatch_queue_subscriber_bytes);
    append("dispatch_queue_peak_bytes", m.peak_stats.conn_dispatch_queue_bytes);
    append("client_read_buffer_peak_bytes", m.peak_stats.conn_read_buf_capacity);

    if (GetFlag(FLAGS_cache_mode)) {
      append("cache_mode", "cache");
      // PHP Symphony needs this field to work.
      append("maxmemory_policy", "eviction");
    } else {
      append("cache_mode", "store");
      // Compatible with redis based frameworks.
      append("maxmemory_policy", "noeviction");
    }

    if (!m.replication_metrics.empty()) {
      ReplicationMemoryStats repl_mem;
      dfly_cmd_->GetReplicationMemoryStats(&repl_mem);
      append("replication_streaming_buffer_bytes", repl_mem.streamer_buf_capacity_bytes_);
      append("replication_full_sync_buffer_bytes", repl_mem.full_sync_buf_bytes_);
    }

    if (IsSaving()) {
      lock_guard lk{save_mu_};
      if (save_bytes_cb_) {
        append("save_buffer_bytes", save_bytes_cb_());
      }
    }
  }

  if (should_enter("STATS")) {
    auto& conn_stats = m.facade_stats.conn_stats;
    auto& reply_stats = m.facade_stats.reply_stats;

    append("total_connections_received", conn_stats.conn_received_cnt);
    append("total_commands_processed", conn_stats.command_cnt);
    append("instantaneous_ops_per_sec", m.qps);
    append("total_pipelined_commands", conn_stats.pipelined_cmd_cnt);
    append("total_net_input_bytes", conn_stats.io_read_bytes);
    append("total_net_output_bytes", reply_stats.io_write_bytes);
    append("instantaneous_input_kbps", -1);
    append("instantaneous_output_kbps", -1);
    append("rejected_connections", -1);
    append("expired_keys", m.events.expired_keys);
    append("evicted_keys", m.events.evicted_keys);
    append("hard_evictions", m.events.hard_evictions);
    append("garbage_checked", m.events.garbage_checked);
    append("garbage_collected", m.events.garbage_collected);
    append("bump_ups", m.events.bumpups);
    append("stash_unloaded", m.events.stash_unloaded);
    append("oom_rejections", m.events.insertion_rejections);
    append("traverse_ttl_sec", m.traverse_ttl_per_sec);
    append("delete_ttl_sec", m.delete_ttl_per_sec);
    append("keyspace_hits", m.events.hits);
    append("keyspace_misses", m.events.misses);
    append("keyspace_mutations", m.events.mutations);
    append("total_reads_processed", conn_stats.io_read_cnt);
    append("total_writes_processed", reply_stats.io_write_cnt);
    append("defrag_attempt_total", m.shard_stats.defrag_attempt_total);
    append("defrag_realloc_total", m.shard_stats.defrag_realloc_total);
    append("defrag_task_invocation_total", m.shard_stats.defrag_task_invocation_total);
    append("reply_count", reply_stats.send_stats[SendStatsType::kRegular].count);
    append("reply_latency_usec", reply_stats.send_stats[SendStatsType::kRegular].total_duration);
    append("reply_batch_count", reply_stats.send_stats[SendStatsType::kBatch].count);
    append("reply_batch_latency_usec",
           reply_stats.send_stats[SendStatsType::kBatch].total_duration);
  }

  if (should_enter("TIERED", true)) {
    append("tiered_entries", total.tiered_entries);
    append("tiered_bytes", total.tiered_size);
    append("tiered_reads", m.tiered_stats.tiered_reads);
    append("tiered_writes", m.tiered_stats.tiered_writes);
    append("tiered_reserved", m.tiered_stats.storage_reserved);
    append("tiered_capacity", m.tiered_stats.storage_capacity);
    append("tiered_aborted_writes", m.tiered_stats.aborted_write_cnt);
    append("tiered_flush_skipped", m.tiered_stats.flush_skip_cnt);
    append("tiered_throttled_writes", m.tiered_stats.throttled_write_cnt);
  }

  if (should_enter("PERSISTENCE", true)) {
    auto save_info = GetLastSaveInfo();

    // when last success save
    append("last_success_save", save_info.save_time);
    append("last_saved_file", save_info.file_name);
    append("last_success_save_duration_sec", save_info.success_duration_sec);

    size_t is_loading = service_.GetGlobalState() == GlobalState::LOADING;
    append("loading", is_loading);

    auto curent_durration_sec =
        start_save_time_ ? (absl::Now() - *start_save_time_) / absl::Seconds(1) : 0;
    append("saving", curent_durration_sec != 0);
    append("current_save_duration_sec", curent_durration_sec);

    for (const auto& k_v : save_info.freq_map) {
      append(StrCat("rdb_", k_v.first), k_v.second);
    }
    append("rdb_changes_since_last_success_save", m.events.update);

    // when last failed save
    append("last_failed_save", save_info.last_error_time);
    append("last_error", save_info.last_error.Format());
    append("last_failed_save_duration_sec", save_info.failed_duration_sec);
  }

  if (should_enter("TRANSACTION", true)) {
    const auto& tc = m.coordinator_stats.tx_type_cnt;
    string val = StrCat("global=", tc[ServerState::GLOBAL], ",normal=", tc[ServerState::NORMAL],
                        ",ooo=", tc[ServerState::OOO], ",quick=", tc[ServerState::QUICK],
                        ",inline=", tc[ServerState::INLINE]);
    append("tx_type_cnt", val);
    val.clear();
    for (unsigned width = 0; width < shard_set->size(); ++width) {
      if (m.coordinator_stats.tx_width_freq_arr[width] > 0) {
        absl::StrAppend(&val, "w", width + 1, "=", m.coordinator_stats.tx_width_freq_arr[width],
                        ",");
      }
    }
    if (!val.empty()) {
      val.pop_back();  // last comma.
      append("tx_width_freq", val);
    }
    append("eval_io_coordination_total", m.coordinator_stats.eval_io_coordination_cnt);
    append("eval_shardlocal_coordination_total",
           m.coordinator_stats.eval_shardlocal_coordination_cnt);
    append("eval_squashed_flushes", m.coordinator_stats.eval_squashed_flushes);
    append("tx_schedule_cancel_total", m.coordinator_stats.tx_schedule_cancel_cnt);
    append("tx_queue_len", m.tx_queue_len);
    append("multi_squash_execution_total", m.coordinator_stats.multi_squash_executions);
    append("multi_squash_execution_hop_usec", m.coordinator_stats.multi_squash_exec_hop_usec);
    append("multi_squash_execution_reply_usec", m.coordinator_stats.multi_squash_exec_reply_usec);
  }

  if (should_enter("REPLICATION")) {
    ServerState& etl = *ServerState::tlocal();

    if (etl.is_master) {
      append("role", "master");
      append("connected_slaves", m.facade_stats.conn_stats.num_replicas);
      const auto& replicas = m.replication_metrics;
      for (size_t i = 0; i < replicas.size(); i++) {
        auto& r = replicas[i];
        // e.g. slave0:ip=172.19.0.3,port=6379,state=full_sync
        append(StrCat("slave", i), StrCat("ip=", r.address, ",port=", r.listening_port,
                                          ",state=", r.state, ",lag=", r.lsn_lag));
      }
      append("master_replid", master_id_);
    } else {
      append("role", "replica");

      // The replica pointer can still be mutated even while master=true,
      // we don't want to drop the replica object in this fiber
      unique_lock lk{replicaof_mu_};
      Replica::Info rinfo = replica_->GetInfo();
      append("master_host", rinfo.host);
      append("master_port", rinfo.port);

      const char* link = rinfo.master_link_established ? "up" : "down";
      append("master_link_status", link);
      append("master_last_io_seconds_ago", rinfo.master_last_io_sec);
      append("master_sync_in_progress", rinfo.full_sync_in_progress);
      append("master_replid", rinfo.master_id);
    }
  }

  if (should_enter("COMMANDSTATS", true)) {
    auto append_sorted = [&append](string_view prefix, auto display) {
      sort(display.begin(), display.end());
      for (const auto& k_v : display) {
        append(StrCat(prefix, k_v.first), k_v.second);
      }
    };

    vector<pair<string_view, string>> commands;
    for (const auto& [name, stats] : m.cmd_stats_map) {
      const auto calls = stats.first, sum = stats.second;
      commands.push_back(
          {name, absl::StrJoin({absl::StrCat("calls=", calls), absl::StrCat("usec=", sum),
                                absl::StrCat("usec_per_call=", static_cast<double>(sum) / calls)},
                               ",")});
    }

    auto unknown_cmd = service_.UknownCmdMap();

    append_sorted("cmdstat_", std::move(commands));
    append_sorted("unknown_",
                  vector<pair<string_view, uint64_t>>(unknown_cmd.cbegin(), unknown_cmd.cend()));
  }

  if (should_enter("MODULES")) {
    append("module",
           "name=ReJSON,ver=20000,api=1,filters=0,usedby=[search],using=[],options=[handle-io-"
           "errors]");
    append("module",
           "name=search,ver=20000,api=1,filters=0,usedby=[],using=[ReJSON],options=[handle-io-"
           "errors]");
  }

  if (should_enter("SEARCH", true)) {
    append("search_memory", m.search_stats.used_memory);
    append("search_num_indices", m.search_stats.num_indices);
    append("search_num_entries", m.search_stats.num_entries);
  }

  if (should_enter("ERRORSTATS", true)) {
    for (const auto& k_v : m.facade_stats.reply_stats.err_count) {
      append(k_v.first, k_v.second);
    }
  }

  if (should_enter("KEYSPACE")) {
    for (size_t i = 0; i < m.db_stats.size(); ++i) {
      const auto& stats = m.db_stats[i];
      bool show = (i == 0) || (stats.key_count > 0);
      if (show) {
        string val = StrCat("keys=", stats.key_count, ",expires=", stats.expire_count,
                            ",avg_ttl=-1");  // TODO
        append(StrCat("db", i), val);
      }
    }
  }

#ifndef __APPLE__
  if (should_enter("CPU")) {
    struct rusage ru, cu, tu;
    getrusage(RUSAGE_SELF, &ru);
    getrusage(RUSAGE_CHILDREN, &cu);
    getrusage(RUSAGE_THREAD, &tu);
    append("used_cpu_sys", StrCat(ru.ru_stime.tv_sec, ".", ru.ru_stime.tv_usec));
    append("used_cpu_user", StrCat(ru.ru_utime.tv_sec, ".", ru.ru_utime.tv_usec));
    append("used_cpu_sys_children", StrCat(cu.ru_stime.tv_sec, ".", cu.ru_stime.tv_usec));
    append("used_cpu_user_children", StrCat(cu.ru_utime.tv_sec, ".", cu.ru_utime.tv_usec));
    append("used_cpu_sys_main_thread", StrCat(tu.ru_stime.tv_sec, ".", tu.ru_stime.tv_usec));
    append("used_cpu_user_main_thread", StrCat(tu.ru_utime.tv_sec, ".", tu.ru_utime.tv_usec));
  }
#endif

  if (should_enter("CLUSTER")) {
    append("cluster_enabled", ClusterConfig::IsEnabledOrEmulated());
  }
  auto* rb = static_cast<RedisReplyBuilder*>(cntx->reply_builder());
  rb->SendVerbatimString(info);
}

void ServerFamily::Hello(CmdArgList args, ConnectionContext* cntx) {
  // If no arguments are provided default to RESP2.
  bool is_resp3 = false;
  bool has_auth = false;
  bool has_setname = false;
  string_view username;
  string_view password;
  string_view clientname;

  if (args.size() > 0) {
    string_view proto_version = ArgS(args, 0);
    is_resp3 = proto_version == "3";
    bool valid_proto_version = proto_version == "2" || is_resp3;
    if (!valid_proto_version) {
      cntx->SendError(UnknownCmd("HELLO", args));
      return;
    }

    for (uint32_t i = 1; i < args.size(); i++) {
      auto sub_cmd = ArgS(args, i);
      auto moreargs = args.size() - 1 - i;
      if (absl::EqualsIgnoreCase(sub_cmd, "AUTH") && moreargs >= 2) {
        has_auth = true;
        username = ArgS(args, i + 1);
        password = ArgS(args, i + 2);
        i += 2;
      } else if (absl::EqualsIgnoreCase(sub_cmd, "SETNAME") && moreargs > 0) {
        has_setname = true;
        clientname = ArgS(args, i + 1);
        i += 1;
      } else {
        cntx->SendError(kSyntaxErr);
        return;
      }
    }
  }

  if (has_auth) {
    if (username == "default" && password == GetPassword()) {
      cntx->authenticated = true;
    } else {
      cntx->SendError(facade::kAuthRejected);
      return;
    }
  }

  if (cntx->req_auth && !cntx->authenticated) {
    cntx->SendError(
        "-NOAUTH HELLO must be called with the client already "
        "authenticated, otherwise the HELLO <proto> AUTH <user> <pass> "
        "option can be used to authenticate the client and "
        "select the RESP protocol version at the same time");
    return;
  }

  if (has_setname) {
    cntx->conn()->SetName(string{clientname});
  }

  auto* rb = static_cast<RedisReplyBuilder*>(cntx->reply_builder());
  int proto_version = 2;
  if (is_resp3) {
    proto_version = 3;
    rb->SetResp3(true);
  } else {
    // Issuing hello 2 again is valid and should switch back to RESP2
    rb->SetResp3(false);
  }

  rb->StartCollection(7, RedisReplyBuilder::MAP);
  rb->SendBulkString("server");
  rb->SendBulkString("redis");
  rb->SendBulkString("version");
  rb->SendBulkString(kRedisVersion);
  rb->SendBulkString("dragonfly_version");
  rb->SendBulkString(GetVersion());
  rb->SendBulkString("proto");
  rb->SendLong(proto_version);
  rb->SendBulkString("id");
  rb->SendLong(cntx->conn()->GetClientId());
  rb->SendBulkString("mode");
  rb->SendBulkString(GetRedisMode());
  rb->SendBulkString("role");
  rb->SendBulkString((*ServerState::tlocal()).is_master ? "master" : "slave");
}

void ServerFamily::ReplicaOfInternal(string_view host, string_view port_sv, ConnectionContext* cntx,
                                     ActionOnConnectionFail on_err) {
  LOG(INFO) << "Replicating " << host << ":" << port_sv;
  unique_lock lk(replicaof_mu_);  // Only one REPLICAOF command can run at a time

  // We should not execute replica of command while loading from snapshot.
  if (ServerState::tlocal()->is_master && service_.GetGlobalState() == GlobalState::LOADING) {
    cntx->SendError("Can not execute during LOADING");
    return;
  }

  // If NO ONE was supplied, just stop the current replica (if it exists)
  if (IsReplicatingNoOne(host, port_sv)) {
    if (!ServerState::tlocal()->is_master) {
      CHECK(replica_);

      SetMasterFlagOnAllThreads(true);  // Flip flag before clearing replica
      replica_->Stop();
      replica_.reset();
    }

    CHECK(service_.SwitchState(GlobalState::LOADING, GlobalState::ACTIVE).first ==
          GlobalState::ACTIVE)
        << "Server is set to replica no one, yet state is not active!";

    return cntx->SendOk();
  }

  uint32_t port;
  if (!absl::SimpleAtoi(port_sv, &port) || port < 1 || port > 65535) {
    cntx->SendError(kInvalidIntErr);
    return;
  }

  // First, switch into the loading state
  if (auto new_state = service_.SwitchState(GlobalState::ACTIVE, GlobalState::LOADING);
      new_state.first != GlobalState::LOADING) {
    LOG(WARNING) << GlobalStateName(new_state.first) << " in progress, ignored";
    cntx->SendError("Invalid state");
    return;
  }

  // If any replication is in progress, stop it, cancellation should kick in immediately
  if (replica_)
    replica_->Stop();

  // If we are called by "Replicate", cntx->transaction will be null but we do not need
  // to flush anything.
  if (cntx->transaction) {
    Drakarys(cntx->transaction, DbSlice::kDbAll);
  }

  // Create a new replica and assing it
  auto new_replica = make_shared<Replica>(string(host), port, &service_, master_id());
  replica_ = new_replica;

  // TODO: disconnect pending blocked clients (pubsub, blocking commands)
  SetMasterFlagOnAllThreads(false);  // Flip flag after assiging replica

  // We proceed connecting below without the lock to allow interrupting the replica immediately.
  // From this point and onward, it should be highly responsive.
  lk.unlock();

  error_code ec{};
  switch (on_err) {
    case ActionOnConnectionFail::kReturnOnError:
      ec = new_replica->Start(cntx);
      break;
    case ActionOnConnectionFail::kContinueReplication:  // set DF to replicate, and forget about it
      new_replica->EnableReplication(cntx);
      break;
  };

  // If the replication attempt failed, clean up global state. The replica should have stopped
  // internally.
  lk.lock();
  if (ec && replica_ == new_replica) {
    service_.SwitchState(GlobalState::LOADING, GlobalState::ACTIVE);
    SetMasterFlagOnAllThreads(true);
    replica_.reset();
  }
}

void ServerFamily::ReplicaOf(CmdArgList args, ConnectionContext* cntx) {
  string_view host = ArgS(args, 0);
  string_view port = ArgS(args, 1);

  ReplicaOfInternal(host, port, cntx, ActionOnConnectionFail::kReturnOnError);
}

void ServerFamily::Replicate(string_view host, string_view port) {
  io::NullSink sink;
  ConnectionContext ctxt{&sink, nullptr};
  ctxt.skip_acl_validation = true;

  ReplicaOfInternal(host, port, &ctxt, ActionOnConnectionFail::kContinueReplication);
}

void ServerFamily::ReplTakeOver(CmdArgList args, ConnectionContext* cntx) {
  VLOG(1) << "ReplTakeOver start";

  unique_lock lk(replicaof_mu_);

  CmdArgParser parser{args};

  auto timeout_sec = parser.Next<float>();
  if (timeout_sec < 0) {
    return cntx->SendError("timeout is negative");
  }

  bool save_flag = static_cast<bool>(parser.Check("SAVE").IgnoreCase());

  if (parser.HasNext())
    return cntx->SendError(absl::StrCat("Unsupported option:", string_view(parser.Next())));

  if (auto err = parser.Error(); err)
    return cntx->SendError(err->MakeReply());

  if (ServerState::tlocal()->is_master)
    return cntx->SendError("Already a master instance");
  auto repl_ptr = replica_;
  CHECK(repl_ptr);

  auto info = replica_->GetInfo();
  if (!info.full_sync_done) {
    return cntx->SendError("Full sync not done");
  }

  std::error_code ec = replica_->TakeOver(ArgS(args, 0), save_flag);
  if (ec)
    return cntx->SendError("Couldn't execute takeover");

  LOG(INFO) << "Takeover successful, promoting this instance to master.";
  service_.proactor_pool().AwaitFiberOnAll(
      [&](util::ProactorBase* pb) { ServerState::tlocal()->is_master = true; });
  replica_->Stop();
  replica_.reset();
  return cntx->SendOk();
}

void ServerFamily::ReplConf(CmdArgList args, ConnectionContext* cntx) {
  if (args.size() % 2 == 1)
    goto err;
  for (unsigned i = 0; i < args.size(); i += 2) {
    DCHECK_LT(i + 1, args.size());
    ToUpper(&args[i]);

    std::string_view cmd = ArgS(args, i);
    std::string_view arg = ArgS(args, i + 1);
    if (cmd == "CAPA") {
      if (arg == "dragonfly" && args.size() == 2 && i == 0) {
        auto [sid, replica_info] = dfly_cmd_->CreateSyncSession(cntx);
        cntx->conn()->SetName(absl::StrCat("repl_ctrl_", sid));

        string sync_id = absl::StrCat("SYNC", sid);
        cntx->conn_state.replication_info.repl_session_id = sid;

        if (!cntx->replica_conn) {
          ServerState::tl_connection_stats()->num_replicas += 1;
        }
        cntx->replica_conn = true;

        // The response for 'capa dragonfly' is: <masterid> <syncid> <numthreads> <version>
        auto* rb = static_cast<RedisReplyBuilder*>(cntx->reply_builder());
        rb->StartArray(4);
        rb->SendSimpleString(master_id_);
        rb->SendSimpleString(sync_id);
        rb->SendLong(replica_info->flows.size());
        rb->SendLong(unsigned(DflyVersion::CURRENT_VER));
        return;
      }
    } else if (cmd == "LISTENING-PORT") {
      uint32_t replica_listening_port;
      if (!absl::SimpleAtoi(arg, &replica_listening_port)) {
        cntx->SendError(kInvalidIntErr);
        return;
      }
      cntx->conn_state.replication_info.repl_listening_port = replica_listening_port;
    } else if (cmd == "CLIENT-ID" && args.size() == 2) {
      std::string client_id{arg};
      auto& pool = service_.proactor_pool();
      pool.AwaitFiberOnAll(
          [&](util::ProactorBase* pb) { ServerState::tlocal()->remote_client_id_ = arg; });
    } else if (cmd == "CLIENT-VERSION" && args.size() == 2) {
      unsigned version;
      if (!absl::SimpleAtoi(arg, &version)) {
        return cntx->SendError(kInvalidIntErr);
      }
      dfly_cmd_->SetDflyClientVersion(cntx, DflyVersion(version));
    } else if (cmd == "ACK" && args.size() == 2) {
      // Don't send error/Ok back through the socket, because we don't want to interleave with
      // the journal writes that we write into the same socket.

      if (!cntx->replication_flow) {
        LOG(ERROR) << "No replication flow assigned";
        return;
      }

      uint64_t ack;
      if (!absl::SimpleAtoi(arg, &ack)) {
        LOG(ERROR) << "Bad int in REPLCONF ACK command! arg=" << arg;
        return;
      }
      VLOG(2) << "Received client ACK=" << ack;
      cntx->replication_flow->last_acked_lsn = ack;
      return;
    } else {
      VLOG(1) << "Error " << cmd << " " << arg << " " << args.size();
      goto err;
    }
  }

  cntx->SendOk();
  return;

err:
  LOG(ERROR) << "Error in receiving command: " << args;
  cntx->SendError(kSyntaxErr);
}

void ServerFamily::Role(CmdArgList args, ConnectionContext* cntx) {
  auto* rb = static_cast<RedisReplyBuilder*>(cntx->reply_builder());
  ServerState& etl = *ServerState::tlocal();
  if (etl.is_master) {
    rb->StartArray(2);
    rb->SendBulkString("master");
    auto vec = dfly_cmd_->GetReplicasRoleInfo();
    rb->StartArray(vec.size());
    for (auto& data : vec) {
      rb->StartArray(3);
      rb->SendBulkString(data.address);
      rb->SendBulkString(absl::StrCat(data.listening_port));
      rb->SendBulkString(data.state);
    }

  } else {
    unique_lock lk{replicaof_mu_};
    Replica::Info rinfo = replica_->GetInfo();
    rb->StartArray(4);
    rb->SendBulkString("replica");
    rb->SendBulkString(rinfo.host);
    rb->SendBulkString(absl::StrCat(rinfo.port));
    if (rinfo.full_sync_done) {
      rb->SendBulkString("stable_sync");
    } else if (rinfo.full_sync_in_progress) {
      rb->SendBulkString("full_sync");
    } else if (rinfo.master_link_established) {
      rb->SendBulkString("preparation");
    } else {
      rb->SendBulkString("connecting");
    }
  }
}

void ServerFamily::Script(CmdArgList args, ConnectionContext* cntx) {
  ToUpper(&args.front());

  script_mgr_->Run(std::move(args), cntx);
}

void ServerFamily::LastSave(CmdArgList args, ConnectionContext* cntx) {
  time_t save_time;
  {
    lock_guard lk(save_mu_);
    save_time = last_save_info_.save_time;
  }
  cntx->SendLong(save_time);
}

void ServerFamily::Latency(CmdArgList args, ConnectionContext* cntx) {
  auto* rb = static_cast<RedisReplyBuilder*>(cntx->reply_builder());
  ToUpper(&args[0]);
  string_view sub_cmd = ArgS(args, 0);

  if (sub_cmd == "LATEST") {
    return rb->SendEmptyArray();
  }

  LOG_FIRST_N(ERROR, 10) << "Subcommand " << sub_cmd << " not supported";
  cntx->SendError(kSyntaxErr);
}

void ServerFamily::ShutdownCmd(CmdArgList args, ConnectionContext* cntx) {
  if (args.size() > 1) {
    cntx->SendError(kSyntaxErr);
    return;
  }

  if (args.size() == 1) {
    auto sub_cmd = ArgS(args, 0);
    if (absl::EqualsIgnoreCase(sub_cmd, "SAVE")) {
    } else if (absl::EqualsIgnoreCase(sub_cmd, "NOSAVE")) {
      save_on_shutdown_ = false;
    } else {
      cntx->SendError(kSyntaxErr);
      return;
    }
  }

  service_.proactor_pool().AwaitFiberOnAll(
      [](ProactorBase* pb) { ServerState::tlocal()->EnterLameDuck(); });

  CHECK_NOTNULL(acceptor_)->Stop();
  cntx->SendOk();
}

void ServerFamily::Dfly(CmdArgList args, ConnectionContext* cntx) {
  dfly_cmd_->Run(args, cntx);
}

void ServerFamily::SlowLog(CmdArgList args, ConnectionContext* cntx) {
  ToUpper(&args[0]);
  string_view sub_cmd = ArgS(args, 0);

  if (sub_cmd == "HELP") {
    string_view help[] = {
        "SLOWLOG <subcommand> [<arg> [value] [opt] ...]. Subcommands are:",
        "GET [<count>]",
        "    Return top <count> entries from the slowlog (default: 10, -1 mean all).",
        "    Entries are made of:",
        "    id, timestamp, time in microseconds, arguments array, client IP and port,",
        "    client name",
        "LEN",
        "    Return the length of the slowlog.",
        "RESET",
        "    Reset the slowlog.",
        "HELP",
        "    Prints this help.",
    };
    auto* rb = static_cast<RedisReplyBuilder*>(cntx->reply_builder());
    rb->SendSimpleStrArr(help);
    return;
  }

  if (sub_cmd == "LEN") {
    vector<int> lengths(service_.proactor_pool().size());
    service_.proactor_pool().AwaitFiberOnAll([&lengths](auto index, auto* context) {
      lengths[index] = ServerState::tlocal()->GetSlowLog().Length();
    });
    int sum = std::accumulate(lengths.begin(), lengths.end(), 0);
    return cntx->SendLong(sum);
  }

  if (sub_cmd == "RESET") {
    service_.proactor_pool().AwaitFiberOnAll(
        [](auto index, auto* context) { ServerState::tlocal()->GetSlowLog().Reset(); });
    return cntx->SendOk();
  }

  if (sub_cmd == "GET") {
    return SlowLogGet(args, cntx, service_, sub_cmd);
  }
  cntx->SendError(UnknownSubCmd(sub_cmd, "SLOWLOG"), kSyntaxErrType);
}

void ServerFamily::Module(CmdArgList args, ConnectionContext* cntx) {
  ToUpper(&args[0]);
  if (ArgS(args, 0) != "LIST")
    return cntx->SendError(kSyntaxErr);

  auto* rb = static_cast<RedisReplyBuilder*>(cntx->reply_builder());
  rb->StartArray(2);

  // Json
  rb->StartCollection(2, RedisReplyBuilder::MAP);
  rb->SendSimpleString("name");
  rb->SendSimpleString("ReJSON");
  rb->SendSimpleString("ver");
  rb->SendLong(20'000);

  // Search
  rb->StartCollection(2, RedisReplyBuilder::MAP);
  rb->SendSimpleString("name");
  rb->SendSimpleString("search");
  rb->SendSimpleString("ver");
  rb->SendLong(20'000);  // we target v2
}

#define HFUNC(x) SetHandler(HandlerFunc(this, &ServerFamily::x))

namespace acl {
constexpr uint32_t kAuth = FAST | CONNECTION;
constexpr uint32_t kBGSave = ADMIN | SLOW | DANGEROUS;
constexpr uint32_t kClient = SLOW | CONNECTION;
constexpr uint32_t kConfig = ADMIN | SLOW | DANGEROUS;
constexpr uint32_t kDbSize = KEYSPACE | READ | FAST;
constexpr uint32_t kDebug = ADMIN | SLOW | DANGEROUS;
constexpr uint32_t kFlushDB = KEYSPACE | WRITE | SLOW | DANGEROUS;
constexpr uint32_t kFlushAll = KEYSPACE | WRITE | SLOW | DANGEROUS;
constexpr uint32_t kInfo = SLOW | DANGEROUS;
constexpr uint32_t kHello = FAST | CONNECTION;
constexpr uint32_t kLastSave = ADMIN | FAST | DANGEROUS;
constexpr uint32_t kLatency = ADMIN | SLOW | DANGEROUS;
constexpr uint32_t kMemory = READ | SLOW;
constexpr uint32_t kSave = ADMIN | SLOW | DANGEROUS;
constexpr uint32_t kShutDown = ADMIN | SLOW | DANGEROUS;
constexpr uint32_t kSlaveOf = ADMIN | SLOW | DANGEROUS;
constexpr uint32_t kReplicaOf = ADMIN | SLOW | DANGEROUS;
constexpr uint32_t kReplTakeOver = DANGEROUS;
constexpr uint32_t kReplConf = ADMIN | SLOW | DANGEROUS;
constexpr uint32_t kRole = ADMIN | FAST | DANGEROUS;
constexpr uint32_t kSlowLog = ADMIN | SLOW | DANGEROUS;
constexpr uint32_t kScript = SLOW | SCRIPTING;
constexpr uint32_t kModule = ADMIN | SLOW | DANGEROUS;
// TODO(check this)
constexpr uint32_t kDfly = ADMIN;
}  // namespace acl

void ServerFamily::Register(CommandRegistry* registry) {
  constexpr auto kReplicaOpts = CO::LOADING | CO::ADMIN | CO::GLOBAL_TRANS;
  constexpr auto kMemOpts = CO::LOADING | CO::READONLY | CO::FAST;
  registry->StartFamily();
  *registry
      << CI{"AUTH", CO::NOSCRIPT | CO::FAST | CO::LOADING, -2, 0, 0, acl::kAuth}.HFUNC(Auth)
      << CI{"BGSAVE", CO::ADMIN | CO::GLOBAL_TRANS, 1, 0, 0, acl::kBGSave}.HFUNC(Save)
      << CI{"CLIENT", CO::NOSCRIPT | CO::LOADING, -2, 0, 0, acl::kClient}.HFUNC(Client)
      << CI{"CONFIG", CO::ADMIN, -2, 0, 0, acl::kConfig}.HFUNC(Config)
      << CI{"DBSIZE", CO::READONLY | CO::FAST | CO::LOADING, 1, 0, 0, acl::kDbSize}.HFUNC(DbSize)
      << CI{"DEBUG", CO::ADMIN | CO::LOADING, -2, 0, 0, acl::kDebug}.HFUNC(Debug)
      << CI{"FLUSHDB", CO::WRITE | CO::GLOBAL_TRANS, 1, 0, 0, acl::kFlushDB}.HFUNC(FlushDb)
      << CI{"FLUSHALL", CO::WRITE | CO::GLOBAL_TRANS, -1, 0, 0, acl::kFlushAll}.HFUNC(FlushAll)
      << CI{"INFO", CO::LOADING, -1, 0, 0, acl::kInfo}.HFUNC(Info)
      << CI{"HELLO", CO::LOADING, -1, 0, 0, acl::kHello}.HFUNC(Hello)
      << CI{"LASTSAVE", CO::LOADING | CO::FAST, 1, 0, 0, acl::kLastSave}.HFUNC(LastSave)
      << CI{"LATENCY", CO::NOSCRIPT | CO::LOADING | CO::FAST, -2, 0, 0, acl::kLatency}.HFUNC(
             Latency)
      << CI{"MEMORY", kMemOpts, -2, 0, 0, acl::kMemory}.HFUNC(Memory)
      << CI{"SAVE", CO::ADMIN | CO::GLOBAL_TRANS, -1, 0, 0, acl::kSave}.HFUNC(Save)
      << CI{"SHUTDOWN", CO::ADMIN | CO::NOSCRIPT | CO::LOADING, -1, 0, 0, acl::kShutDown}.HFUNC(
             ShutdownCmd)
      << CI{"SLAVEOF", kReplicaOpts, 3, 0, 0, acl::kSlaveOf}.HFUNC(ReplicaOf)
      << CI{"REPLICAOF", kReplicaOpts, 3, 0, 0, acl::kReplicaOf}.HFUNC(ReplicaOf)
      << CI{"REPLTAKEOVER", CO::ADMIN | CO::GLOBAL_TRANS, -2, 0, 0, acl::kReplTakeOver}.HFUNC(
             ReplTakeOver)
      << CI{"REPLCONF", CO::ADMIN | CO::LOADING, -1, 0, 0, acl::kReplConf}.HFUNC(ReplConf)
      << CI{"ROLE", CO::LOADING | CO::FAST | CO::NOSCRIPT, 1, 0, 0, acl::kRole}.HFUNC(Role)
      << CI{"SLOWLOG", CO::ADMIN | CO::FAST, -2, 0, 0, acl::kSlowLog}.HFUNC(SlowLog)
      << CI{"SCRIPT", CO::NOSCRIPT | CO::NO_KEY_TRANSACTIONAL, -2, 0, 0, acl::kScript}.HFUNC(Script)
      << CI{"DFLY", CO::ADMIN | CO::GLOBAL_TRANS | CO::HIDDEN, -2, 0, 0, acl::kDfly}.HFUNC(Dfly)
      << CI{"MODULE", CO::ADMIN, 2, 0, 0, acl::kModule}.HFUNC(Module);
}

}  // namespace dfly
