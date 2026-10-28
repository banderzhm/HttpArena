import std;
import cnetmod.core;
import cnetmod.core.log;
import cnetmod.core.ssl;
import cnetmod.coro.cancel;
import cnetmod.coro.task;
import cnetmod.executor.pool;
import cnetmod.io.io_context;
import cnetmod.protocol.http;
import cnetmod.protocol.http.reverse_proxy;
import cnetmod.protocol.http.v3.server;
import cnetmod.protocol.http.v3.session;
import cnetmod.protocol.http.v3.static_files;

namespace {

namespace http = cnetmod::http;

struct gateway_state {
  std::unordered_map<cnetmod::io_context *, http::reverse_proxy *> by_loop;
  std::vector<std::unique_ptr<http::reverse_proxy>> proxies;
};

auto parse_positive(std::string_view text, unsigned &value) -> bool {
  const auto [end, error] =
      std::from_chars(text.data(), text.data() + text.size(), value);
  return error == std::errc{} && end == text.data() + text.size() && value > 0;
}

auto configure_transport(http::v3::http3_server &server) -> bool {
  if (const auto configured = server.set_udp_segmentation_offload(true);
      !configured) {
    logger::error{"HTTP/3 UDP offload setup failed: {}",
                  configured.error().message()};
    return false;
  }
  if (const auto configured =
          server.set_idle_timeout(std::chrono::milliseconds{1500});
      !configured) {
    logger::error{"HTTP/3 idle-timeout setup failed: {}",
                  configured.error().message()};
    return false;
  }
  if (const auto configured =
          server.set_path_mtu_discovery(1472U, std::chrono::milliseconds{1});
      !configured) {
    logger::error{"HTTP/3 path-MTU setup failed: {}",
                  configured.error().message()};
    return false;
  }
  if (const auto configured = server.set_max_udp_payload_size(1472U);
      !configured) {
    logger::error{"HTTP/3 UDP payload setup failed: {}",
                  configured.error().message()};
    return false;
  }
  if (const auto configured = server.set_qpack_settings(4096U, 100U);
      !configured) {
    logger::error{"HTTP/3 QPACK setup failed: {}",
                  configured.error().message()};
    return false;
  }
  if (const auto configured = server.set_qpack_encoder_table_limit(0U);
      !configured) {
    logger::error{"HTTP/3 QPACK encoder setup failed: {}",
                  configured.error().message()};
    return false;
  }
  return true;
}

auto make_proxy(cnetmod::io_context &loop, std::string upstream,
                std::size_t maximum_clients)
    -> std::expected<std::unique_ptr<http::reverse_proxy>, std::error_code> {
  http::reverse_proxy_options options;
  options.upstream = std::move(upstream);
  options.forwarded_headers = http::forwarded_header_policy::omit;
  options.pool.maximum_clients = maximum_clients;
  options.pool.maximum_waiters = maximum_clients * 2U;
  options.pool.acquire_timeout = std::chrono::seconds{1};
  options.pool.client.connect_timeout = std::chrono::seconds{1};
  options.pool.client.request_timeout = std::chrono::seconds{5};
  options.pool.client.version_pref = http::http_version_preference::http1_only;
  return http::reverse_proxy::create(loop, std::move(options));
}

auto proxy_for(const std::shared_ptr<gateway_state> &state)
    -> http::reverse_proxy * {
  thread_local const gateway_state *cached_state{};
  thread_local http::reverse_proxy *cached_proxy{};
  if (cached_state == state.get())
    return cached_proxy;

  const auto found = state->by_loop.find(cnetmod::io_context::current());
  if (found == state->by_loop.end())
    return nullptr;
  cached_state = state.get();
  cached_proxy = found->second;
  return cached_proxy;
}

} // namespace

auto main(int argc, char **argv) -> int {
  cnetmod::net_init network;
  logger::init("cnetmod-gateway-h3", logger::level::warn);
  if (argc != 7) {
    logger::error{
        "usage: gateway_h3 CERT KEY PORT WORKERS STATIC_DIR UPSTREAM"};
    return 2;
  }

  unsigned port{}, workers{};
  if (!parse_positive(argv[3], port) || port > 65535 ||
      !parse_positive(argv[4], workers))
    return 2;

  auto tls = cnetmod::ssl_context::quic_server();
  if (!tls)
    return 1;
  tls->configure_alpn_server({"h3"});
  if (!tls->load_cert_file(argv[1]) || !tls->load_key_file(argv[2]))
    return 1;

  auto static_files = http::v3::serve_dir({.root = argv[5]});
  cnetmod::server_context context{workers, 1U};
  auto state = std::make_shared<gateway_state>();
  state->proxies.reserve(context.worker_count());
  state->by_loop.reserve(context.worker_count());
  constexpr std::size_t benchmark_concurrency = 256U * 32U;
  const auto clients_per_loop = std::max<std::size_t>(
      128U, (benchmark_concurrency + context.worker_count() - 1U) /
                context.worker_count());
  for (auto *loop : context.worker_ios()) {
    auto proxy = make_proxy(*loop, argv[6], clients_per_loop);
    if (!proxy) {
      logger::error{"failed to create upstream proxy: {}",
                    proxy.error().message()};
      return 1;
    }
    state->by_loop.emplace(loop, proxy->get());
    state->proxies.push_back(std::move(*proxy));
  }

  http::v3::async_server_request_handler handler =
      [state, static_files = std::move(static_files)](
          http::v3::http3_request &request, http::v3::http3_response &response,
          cnetmod::cancel_token &cancellation)
      -> cnetmod::task<std::expected<void, std::error_code>> {
    if (static_files) {
      auto served = static_files(request, response);
      if (!served)
        co_return std::unexpected(served.error());
      if (*served)
        co_return {};
    }

    auto *proxy = proxy_for(state);
    if (!proxy)
      co_return std::unexpected(
          std::make_error_code(std::errc::state_not_recoverable));
    co_return co_await proxy->forward(request, response, cancellation);
  };

  const cnetmod::endpoint endpoint{cnetmod::ipv4_address::any(),
                                   static_cast<std::uint16_t>(port)};
  auto server =
      http::v3::make_http3_server(context, *tls, endpoint, std::move(handler));
  if (!configure_transport(*server))
    return 1;
  if (const auto started = server->start(); !started) {
    logger::error{"HTTP/3 gateway start failed: {}", started.error().message()};
    return 1;
  }

  context.run();
  logger::shutdown();
  return 0;
}
