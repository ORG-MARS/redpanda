#pragma once

#include "rpc/netbuf.h"
#include "rpc/parse_utils.h"
#include "rpc/types.h"

#include <seastar/core/gate.hh>
#include <seastar/core/iostream.hh>
#include <seastar/net/api.hh>

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <utility>

namespace rpc {
class client_context_impl;

class client {
public:
    using promise_t = promise<std::unique_ptr<streaming_context>>;
    explicit client(client_configuration c);
    client(client&&) noexcept = default;
    virtual ~client() noexcept = default;
    future<> connect();
    future<> stop();
    void shutdown();
    [[gnu::always_inline]] bool is_valid() {
        return _connected && !_in.eof();
    }
    virtual future<std::unique_ptr<streaming_context>> send(netbuf);

    template<typename Input, typename Output>
    future<client_context<Output>> send_typed(Input, uint32_t);

    const client_configuration cfg;

private:
    friend client_context_impl;

    future<> do_connect();
    future<> do_reads();
    future<> dispatch(header);
    void fail_outstanding_futures();

    semaphore _memory;
    std::unordered_map<uint32_t, promise_t> _correlations;
    connected_socket _fd;
    input_stream<char> _in;
    output_stream<char> _out;
    gate _dispatch_gate;
    bool _connected = false;
    uint32_t _correlation_idx{0};
};

template<typename Input, typename Output>
inline future<client_context<Output>>
client::send_typed(Input r, uint32_t method_id) {
    auto b = rpc::netbuf();
    b.serialize_type(r);
    b.set_service_method_id(method_id);
    return send(std::move(b))
      .then([this](std::unique_ptr<streaming_context> sctx) mutable {
          return parse_type<Output>(_in, sctx->get_header())
            .then([sctx = std::move(sctx)](Output o) {
                sctx->signal_body_parse();
                using ctx_t = rpc::client_context<Output>;
                // TODO - don't copy the header
                ctx_t ctx(sctx->get_header());
                std::swap(ctx.data, o);
                return make_ready_future<ctx_t>(std::move(ctx));
            });
      });
}

} // namespace rpc