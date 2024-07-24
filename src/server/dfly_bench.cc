// Copyright 2023, DragonflyDB authors.  All rights reserved.
// See LICENSE for licensing terms.
//

#include <absl/random/random.h>
#include <absl/strings/match.h>
#include <absl/strings/str_cat.h>
#include <absl/strings/str_format.h>
#include <absl/strings/str_split.h>

#include <queue>

#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "base/histogram.h"
#include "base/init.h"
#include "base/random.h"
#include "base/zipf_gen.h"
#include "facade/redis_parser.h"
#include "io/io_buf.h"
#include "util/fibers/dns_resolve.h"
#include "util/fibers/pool.h"
#include "util/fibers/uring_socket.h"

// A load-test for DragonflyDB that fixes coordinated omission problem.

using std::string;

ABSL_FLAG(uint16_t, p, 6379, "Server port");
ABSL_FLAG(uint32_t, c, 20, "Number of connections per thread");
ABSL_FLAG(uint32_t, qps, 20, "QPS schedule at which the generator sends requests to the server");
ABSL_FLAG(uint32_t, n, 1000, "Number of requests to send per connection");
ABSL_FLAG(uint32_t, d, 16, "Value size in bytes ");
ABSL_FLAG(string, h, "localhost", "server hostname/ip");
ABSL_FLAG(uint64_t, key_minimum, 0, "Min value for keys used");
ABSL_FLAG(uint64_t, key_maximum, 50'000'000, "Max value for keys used");
ABSL_FLAG(string, key_prefix, "key:", "keys prefix");
ABSL_FLAG(string, key_dist, "U", "U for uniform, N for normal, Z for zipfian, S for sequential");
ABSL_FLAG(double, zipf_alpha, 0.99, "zipfian alpha parameter");
ABSL_FLAG(uint64_t, seed, 42, "A seed for random data generation");
ABSL_FLAG(uint64_t, key_stddev, 0,
          "Standard deviation for non-uniform distribution, 0 chooses"
          " a default value of (max-min)/6");
ABSL_FLAG(string, ratio, "1:10", "Set:Get ratio");
ABSL_FLAG(string, command, "", "custom command with __key__ placeholder for keys");
ABSL_FLAG(string, P, "", "protocol can be empty (for RESP) or memcache_text");

using namespace std;
using namespace util;
using absl::GetFlag;
using facade::RedisParser;
using facade::RespVec;
using tcp = ::boost::asio::ip::tcp;

constexpr string_view kKeyPlaceholder = "__key__"sv;

thread_local base::Xoroshiro128p bit_gen;

#if __INTELLISENSE__
#pragma diag_suppress 144
#endif

enum Protocol { RESP, MC_TEXT } protocol;
enum DistType { UNIFORM, NORMAL, ZIPFIAN, SEQUENTIAL } dist_type{UNIFORM};

class KeyGenerator {
 public:
  KeyGenerator(uint32_t min, uint32_t max);

  string operator()();

 private:
  string prefix_;
  uint64_t min_, max_, range_;
  uint64_t seq_cursor_;
  double stddev_ = 1.0 / 6;
  optional<base::ZipfianGenerator> zipf_;
};

class CommandGenerator {
 public:
  CommandGenerator(KeyGenerator* keygen);

  string Next();

  bool might_hit() const {
    return might_hit_;
  }

 private:
  void FillSet(string_view key);
  void FillGet(string_view key);

  KeyGenerator* keygen_;
  uint32_t ratio_set_ = 0, ratio_get_ = 0;
  string command_;
  string cmd_;
  std::vector<size_t> key_indices_;
  string value_;
  bool might_hit_ = false;
};

CommandGenerator::CommandGenerator(KeyGenerator* keygen) : keygen_(keygen) {
  command_ = GetFlag(FLAGS_command);
  value_ = string(GetFlag(FLAGS_d), 'a');

  if (command_.empty()) {
    pair<string, string> ratio_str = absl::StrSplit(GetFlag(FLAGS_ratio), ':');
    CHECK(absl::SimpleAtoi(ratio_str.first, &ratio_set_));
    CHECK(absl::SimpleAtoi(ratio_str.second, &ratio_get_));
  } else {
    for (size_t pos = 0; (pos = command_.find(kKeyPlaceholder, pos)) != string::npos;
         pos += kKeyPlaceholder.size()) {
      key_indices_.push_back(pos);
    }
  }
}

string CommandGenerator::Next() {
  cmd_.clear();
  string key;
  if (command_.empty()) {
    key = (*keygen_)();

    if (absl::Uniform(bit_gen, 0U, ratio_get_ + ratio_set_) < ratio_set_) {
      FillSet(key);
      might_hit_ = false;
    } else {
      FillGet(key);
      might_hit_ = true;
    }
  } else {
    size_t last_pos = 0;
    for (size_t pos : key_indices_) {
      key = (*keygen_)();
      absl::StrAppend(&cmd_, command_.substr(last_pos, pos - last_pos), key);
      last_pos = pos + kKeyPlaceholder.size();
    }
    absl::StrAppend(&cmd_, command_.substr(last_pos), "\r\n");
  }
  return cmd_;
}

void CommandGenerator::FillSet(string_view key) {
  if (protocol == RESP) {
    absl::StrAppend(&cmd_, "set ", key, " ", value_, "\r\n");
  } else {
    DCHECK_EQ(protocol, MC_TEXT);
    absl::StrAppend(&cmd_, "set ", key, " 0 0 ", value_.size(), "\r\n");
    absl::StrAppend(&cmd_, value_, "\r\n");
  }
}

void CommandGenerator::FillGet(string_view key) {
  absl::StrAppend(&cmd_, "get ", key, "\r\n");
}

struct ClientStats {
  base::Histogram hist;

  uint64_t num_responses = 0;
  uint64_t hit_count = 0;
  uint64_t hit_opportunities = 0;
  uint64_t num_errors = 0;
};

// Per connection driver.
class Driver {
 public:
  explicit Driver(uint32_t num_reqs, ClientStats* stats, ProactorBase* p)
      : num_reqs_(num_reqs), stats_(*stats) {
    socket_.reset(p->CreateSocket());
  }

  Driver(const Driver&) = delete;
  Driver(Driver&&) = default;
  Driver& operator=(Driver&&) = default;

  void Connect(unsigned index, const tcp::endpoint& ep);
  void Run(uint64_t cycle_ns, CommandGenerator* cmd_gen);

 private:
  void PopRequest();
  void ReceiveFb();
  void ParseRESP(facade::RedisParser* parser, io::IoBuf* io_buf);

  struct Req {
    uint64_t start;
    bool might_hit;
  };

  uint32_t num_reqs_;
  ClientStats& stats_;
  unique_ptr<FiberSocketBase> socket_;
  queue<Req> reqs_;
  fb2::CondVarAny cnd_;
};

// Per thread client.
class TLocalClient {
 public:
  explicit TLocalClient(ProactorBase* p) : p_(p) {
    drivers_.resize(GetFlag(FLAGS_c));
    for (auto& driver : drivers_) {
      driver.reset(new Driver{GetFlag(FLAGS_n), &stats, p_});
    }
  }

  TLocalClient(const TLocalClient&) = delete;

  void Connect(tcp::endpoint ep);
  void Run(uint32_t key_min, uint32_t key_max, uint64_t cycle_ns);

  ClientStats stats;

 private:
  ProactorBase* p_;
  vector<unique_ptr<Driver>> drivers_;
};

KeyGenerator::KeyGenerator(uint32_t min, uint32_t max)
    : min_(min), max_(max), range_(max - min + 1) {
  prefix_ = GetFlag(FLAGS_key_prefix);
  CHECK_GT(range_, 0u);

  seq_cursor_ = min_;
  switch (dist_type) {
    case NORMAL: {
      uint64_t stddev = GetFlag(FLAGS_key_stddev);
      if (stddev != 0) {
        stddev_ = double(stddev) / double(range_);
      }
      break;
    }
    case ZIPFIAN:
      zipf_.emplace(min, max, GetFlag(FLAGS_zipf_alpha));
      break;
    default:;
  }
}

string KeyGenerator::operator()() {
  uint64_t key_suffix{0};
  switch (dist_type) {
    case UNIFORM:
      key_suffix = absl::Uniform(bit_gen, min_, max_);
      break;
    case NORMAL: {
      double val = absl::Gaussian(bit_gen, 0.5, stddev_);
      key_suffix = min_ + uint64_t(val * range_);
      break;
    }
    case ZIPFIAN:
      key_suffix = zipf_->Next(bit_gen);
      break;
    case SEQUENTIAL:
      key_suffix = seq_cursor_++;
      if (seq_cursor_ > max_)
        seq_cursor_ = min_;
      break;
  }

  return absl::StrCat(prefix_, key_suffix);
}

void Driver::Connect(unsigned index, const tcp::endpoint& ep) {
  VLOG(2) << "Connecting " << index;
  error_code ec = socket_->Connect(ep);
  CHECK(!ec) << "Could not connect to " << ep << " " << ec;
}

void Driver::Run(uint64_t cycle_ns, CommandGenerator* cmd_gen) {
  auto receive_fb = MakeFiber([this] { ReceiveFb(); });

  int64_t next_invocation = absl::GetCurrentTimeNanos();

  const absl::Time start = absl::Now();

  for (unsigned i = 0; i < num_reqs_; ++i) {
    int64_t now = absl::GetCurrentTimeNanos();

    if (cycle_ns) {
      int64_t sleep_ns = next_invocation - now;
      if (sleep_ns > 0) {
        VLOG(5) << "Sleeping for " << sleep_ns << "ns";
        ThisFiber::SleepFor(chrono::nanoseconds(sleep_ns));
      } else {
        VLOG(5) << "Behind QPS schedule";
      }
      next_invocation += cycle_ns;
    } else {
      // Coordinated omission.

      fb2::NoOpLock lk;
      cnd_.wait(lk, [this] { return reqs_.empty(); });
    }
    string cmd = cmd_gen->Next();

    Req req;
    req.start = absl::GetCurrentTimeNanos();
    req.might_hit = cmd_gen->might_hit();

    reqs_.push(req);

    error_code ec = socket_->Write(io::Buffer(cmd));
    if (ec && FiberSocketBase::IsConnClosed(ec)) {
      // TODO: report failure
      VLOG(1) << "Connection closed";
      break;
    }
    CHECK(!ec) << ec.message();
  }

  const absl::Time finish = absl::Now();
  VLOG(1) << "Done queuing " << num_reqs_ << " requests, which took " << finish - start
          << ". Waiting for server processing";

  // TODO: to change to a condvar or something.
  while (!reqs_.empty()) {
    ThisFiber::SleepFor(1ms);
  }

  socket_->Shutdown(SHUT_RDWR);  // breaks the receive fiber.
  receive_fb.Join();
  std::ignore = socket_->Close();
}

static string_view FindLine(io::Bytes buf) {
  if (buf.size() < 2)
    return {};
  for (unsigned i = 0; i < buf.size() - 1; ++i) {
    if (buf[i] == '\r' && buf[i + 1] == '\n') {
      return io::View(buf.subspan(0, i + 2));
    }
  }
  return {};
};

void Driver::PopRequest() {
  uint64_t now = absl::GetCurrentTimeNanos();
  uint64_t usec = (now - reqs_.front().start) / 1000;
  stats_.hist.Add(usec);
  stats_.hit_opportunities += reqs_.front().might_hit;

  reqs_.pop();
  if (reqs_.empty()) {
    cnd_.notify_one();
  }
  ++stats_.num_responses;
}

void Driver::ReceiveFb() {
  facade::RedisParser parser{1 << 16, false};
  io::IoBuf io_buf{512};

  unsigned blob_len = 0;

  while (true) {
    io_buf.EnsureCapacity(256);
    auto buf = io_buf.AppendBuffer();
    VLOG(2) << "Socket read: " << reqs_.size();

    ::io::Result<size_t> recv_sz = socket_->Recv(buf);
    if (!recv_sz && FiberSocketBase::IsConnClosed(recv_sz.error())) {
      break;
    }
    CHECK(recv_sz) << recv_sz.error().message();
    io_buf.CommitWrite(*recv_sz);

    if (protocol == RESP) {
      ParseRESP(&parser, &io_buf);
    } else {
      // MC_TEXT
      while (true) {
        string_view line = FindLine(io_buf.InputBuffer());
        if (line.empty())
          break;
        CHECK_EQ(line.back(), '\n');
        if (line == "STORED\r\n" || line == "END\r\n") {
          PopRequest();
          blob_len = 0;
        } else if (absl::StartsWith(line, "VALUE")) {
          // last token is a blob length.
          auto it = line.rbegin();
          while (it != line.rend() && *it != ' ')
            ++it;
          size_t len = it - line.rbegin() - 2;
          const char* start = &(*it) + 1;
          if (!absl::SimpleAtoi(string(start, len), &blob_len)) {
            LOG(ERROR) << "Invalid blob len " << line;
            return;
          }
          ++stats_.hit_count;
        } else if (absl::StartsWith(line, "SERVER_ERROR")) {
          ++stats_.num_errors;
          PopRequest();
          blob_len = 0;
        } else {
          auto handle = socket_->native_handle();
          CHECK_EQ(blob_len + 2, line.size()) << line;
          blob_len = 0;
          VLOG(2) << "Got line " << handle << ": " << line;
        }
        io_buf.ConsumeInput(line.size());
      }
    }
  }
  VLOG(1) << "ReceiveFb done";
}

void Driver::ParseRESP(facade::RedisParser* parser, io::IoBuf* io_buf) {
  uint32_t consumed = 0;
  RedisParser::Result result = RedisParser::OK;
  RespVec parse_args;

  do {
    result = parser->Parse(io_buf->InputBuffer(), &consumed, &parse_args);
    if (result == RedisParser::OK && !parse_args.empty()) {
      if (parse_args[0].type == facade::RespExpr::ERROR) {
        ++stats_.num_errors;
      } else if (reqs_.front().might_hit && parse_args[0].type != facade::RespExpr::NIL) {
        ++stats_.hit_count;
      }
      parse_args.clear();
      PopRequest();
    }
    io_buf->ConsumeInput(consumed);
  } while (result == RedisParser::OK);
}

void TLocalClient::Connect(tcp::endpoint ep) {
  VLOG(2) << "Connecting client...";
  vector<fb2::Fiber> fbs(drivers_.size());

  for (size_t i = 0; i < fbs.size(); ++i) {
    fbs[i] = MakeFiber([&, i] {
      ThisFiber::SetName(absl::StrCat("connect/", i));
      drivers_[i]->Connect(i, ep);
    });
  }

  for (auto& fb : fbs)
    fb.Join();
}

void TLocalClient::Run(uint32_t key_min, uint32_t key_max, uint64_t cycle_ns) {
  KeyGenerator key_gen(key_min, key_max);
  CommandGenerator cmd_gen(&key_gen);

  vector<fb2::Fiber> fbs(drivers_.size());
  for (size_t i = 0; i < fbs.size(); ++i) {
    fbs[i] = fb2::Fiber(absl::StrCat("run/", i), [&, i] { drivers_[i]->Run(cycle_ns, &cmd_gen); });
  }

  for (auto& fb : fbs)
    fb.Join();

  VLOG(1) << "Total hits: " << stats.hit_count;
}

thread_local unique_ptr<TLocalClient> client;

void WatchFiber(absl::Time start_time, atomic_bool* finish_signal, ProactorPool* pp) {
  fb2::Mutex mutex;

  absl::Time last_print;  // initialized to epoch time.
  uint64_t num_last_resp_cnt = 0;

  uint64_t resp_goal = GetFlag(FLAGS_c) * pp->size() * GetFlag(FLAGS_n);

  while (*finish_signal == false) {
    // we sleep with resolution of 1s but print with lower frequency to be more responsive
    // when benchmark finishes.
    ThisFiber::SleepFor(1s);
    absl::Time now = absl::Now();
    if (now - last_print > absl::Seconds(5)) {
      uint64_t num_resp = 0;
      uint64_t num_errors = 0;

      pp->AwaitFiberOnAll([&](auto* p) {
        unique_lock lk(mutex);

        num_resp += client->stats.num_responses;
        num_errors += client->stats.num_errors;
        lk.unlock();
      });

      uint64_t total_ms = (now - start_time) / absl::Milliseconds(1);
      uint64_t period_ms = (now - last_print) / absl::Milliseconds(1);
      uint64_t period_resp_cnt = num_resp - num_last_resp_cnt;
      double done_perc = double(num_resp) * 100 / resp_goal;

      CONSOLE_INFO << total_ms / 1000 << "s: " << absl::StrFormat("%.1f", done_perc)
                   << "% done, effective RPS(now/accumulated): "
                   << period_resp_cnt * 1000 / period_ms << "/" << num_resp * 1000 / total_ms
                   << ", errors: " << num_errors;

      last_print = now;
      num_last_resp_cnt = num_resp;
    }
  }
}

int main(int argc, char* argv[]) {
  MainInitGuard guard(&argc, &argv);

  unique_ptr<ProactorPool> pp;
  pp.reset(fb2::Pool::IOUring(256));
  pp->Run();

  string proto_str = GetFlag(FLAGS_P);
  if (proto_str == "memcache_text") {
    protocol = MC_TEXT;
  } else {
    CHECK(proto_str.empty());
    protocol = RESP;
  }

  string dist = GetFlag(FLAGS_key_dist);

  if (dist == "U") {
    dist_type = UNIFORM;
  } else if (dist == "N") {
    dist_type = NORMAL;
  } else if (dist == "Z") {
    dist_type = ZIPFIAN;
  } else if (dist == "S") {
    dist_type = SEQUENTIAL;
  } else {
    LOG(FATAL) << "Unknown distribution type: " << dist;
  }

  auto* proactor = pp->GetNextProactor();
  char ip_addr[128];

  error_code ec =
      proactor->Await([&] { return fb2::DnsResolve(GetFlag(FLAGS_h), 2000, ip_addr, proactor); });
  CHECK(!ec) << "Could not resolve " << GetFlag(FLAGS_h) << " " << ec;

  auto address = ::boost::asio::ip::make_address(ip_addr);
  tcp::endpoint ep{address, GetFlag(FLAGS_p)};

  LOG(INFO) << "Connecting threads";
  pp->AwaitFiberOnAll([&](unsigned index, auto* p) {
    base::SplitMix64 seed_mix(GetFlag(FLAGS_seed) + index * 0x6a45554a264d72bULL);
    auto seed = seed_mix();
    VLOG(1) << "Seeding bitgen with seed " << seed;
    bit_gen.seed(seed);
    client = make_unique<TLocalClient>(p);
    client->Connect(ep);
  });

  const uint32_t key_minimum = GetFlag(FLAGS_key_minimum);
  const uint32_t key_maximum = GetFlag(FLAGS_key_maximum);
  CHECK_LE(key_minimum, key_maximum);

  uint32_t thread_key_step = 0;
  const uint32_t qps = GetFlag(FLAGS_qps);
  const int64_t interval = qps ? 1000000000LL / qps : 0;
  uint64_t num_reqs = GetFlag(FLAGS_n);
  uint64_t total_conn_num = GetFlag(FLAGS_c) * pp->size();
  uint64_t total_requests = num_reqs * total_conn_num;

  if (dist_type == SEQUENTIAL) {
    thread_key_step = std::max(1UL, (key_maximum - key_minimum + 1) / pp->size());
    if (total_requests > (key_maximum - key_minimum)) {
      CONSOLE_INFO << "Warning: only " << key_maximum - key_minimum
                   << " unique entries will be accessed with " << total_requests
                   << " total requests";
    }
  }

  CONSOLE_INFO << "Running " << pp->size() << " threads, sending " << num_reqs
               << " requests per each connection, or " << total_requests << " requests overall";

  if (interval) {
    CONSOLE_INFO << "At a rate of " << GetFlag(FLAGS_qps)
                 << " rps per connection, i.e. request every " << interval / 1000 << "us";
    CONSOLE_INFO << "Overall scheduled RPS: " << qps * total_conn_num;
  } else {
    CONSOLE_INFO << "Coordinated omission mode - the rate is determined by the server";
  }
  const absl::Time start_time = absl::Now();
  atomic_bool finish{false};
  auto watch_fb =
      pp->GetNextProactor()->LaunchFiber([&] { WatchFiber(start_time, &finish, pp.get()); });

  pp->AwaitFiberOnAll([&](unsigned index, auto* p) {
    uint32_t key_max = (thread_key_step > 0 && index + 1 < pp->size())
                           ? key_minimum + (index + 1) * thread_key_step - 1
                           : key_maximum;
    client->Run(key_minimum + index * thread_key_step, key_max, interval);
  });
  absl::Duration duration = absl::Now() - start_time;
  finish.store(true);
  watch_fb.Join();

  fb2::Mutex mutex;
  base::Histogram hist;

  LOG(INFO) << "Resetting all threads";
  uint64_t hit_opportunities = 0, hit_count = 0, num_errors = 0, num_responses = 0;

  pp->AwaitFiberOnAll([&](auto* p) {
    unique_lock lk(mutex);
    hist.Merge(client->stats.hist);

    hit_opportunities += client->stats.hit_opportunities;
    hit_count += client->stats.hit_count;
    num_errors += client->stats.num_errors;
    num_responses += client->stats.num_responses;
    lk.unlock();
    client.reset();
  });

  CONSOLE_INFO << "\nTotal time: " << duration << ". Overall number of requests: " << num_responses
               << ", QPS: " << num_responses / (duration / absl::Seconds(1));

  if (num_errors) {
    CONSOLE_INFO << "Got " << num_errors << " error responses!";
  }

  CONSOLE_INFO << "Latency summary, all times are in usec:\n" << hist.ToString();
  if (hit_opportunities) {
    CONSOLE_INFO << "----------------------------------\nHit rate: "
                 << 100 * double(hit_count) / double(hit_opportunities) << "%\n";
  }
  pp->Stop();

  return 0;
}
