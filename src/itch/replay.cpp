#include "enginex/itch/replay.hpp"

#include <vector>

namespace enginex::itch {

namespace {

class Driver {
public:
    Driver(BookBuilder& book, const ReplayOptions& options, ReplayResult& result)
        : book_(book), options_(options), r_(result) {}

    // Returns false once replay must stop.
    bool feed(std::span<const std::uint8_t> chunk) {
        if (r_.stopped) {
            return false;
        }
        r_.bytes += chunk.size();
        const FrameError e = framer_.feed(chunk, [&](std::span<const std::uint8_t> payload) {
            if (!r_.stopped) {
                on_payload(payload);
            }
        });
        if (e != FrameError::None) {
            frame_failed(e);
        }
        return !r_.stopped;
    }

    void finish() {
        r_.frames = framer_.frames();
        if (r_.stopped) {
            return;
        }
        if (const FrameError e = framer_.finish(); e != FrameError::None) {
            frame_failed(e);
        }
    }

private:
    void on_payload(std::span<const std::uint8_t> payload) {
        ++frame_no_;
        const DecodeResult d = decode(payload);
        if (const auto* err = std::get_if<DecodeError>(&d)) {
            ++r_.decode_errors[static_cast<std::size_t>(*err)];
            if (*err == DecodeError::UnknownType) {
                ++r_.unknown_types[payload[0]];
            }
            if (options_.strict) {
                stop("decode error " + std::string(to_string(*err)) + " (type byte " +
                     std::to_string(payload.empty() ? 0 : payload[0]) + ", length " +
                     std::to_string(payload.size()) + ")");
            }
            return;
        }
        const Message& m = std::get<Message>(d);
        ++r_.decoded;
        const ApplyOutcome out = book_.apply(m);
        if (options_.on_message) {
            options_.on_message(m, out);
        }
        if (options_.strict && out.anomaly != Anomaly::None) {
            stop("book anomaly " + std::string(to_string(out.anomaly)) + " on " +
                 std::string(type_name(header_of(m).type)));
        }
    }

    void frame_failed(FrameError e) {
        r_.frame_error = e;
        r_.frame_error_offset = framer_.error_offset();
        if (!r_.stopped) {
            r_.stopped = "framing error " + std::string(to_string(e)) + " at byte offset " +
                         std::to_string(framer_.error_offset());
            r_.stopped_at_message = frame_no_ + 1;
        }
    }

    void stop(std::string why) {
        r_.stopped = std::move(why);
        r_.stopped_at_message = frame_no_;
    }

    BookBuilder& book_;
    const ReplayOptions& options_;
    ReplayResult& r_;
    StreamFramer framer_;
    std::uint64_t frame_no_ = 0;
};

}  // namespace

ReplayResult replay(std::span<const std::uint8_t> file, BookBuilder& book,
                    const ReplayOptions& options) {
    ReplayResult r;
    Driver d(book, options, r);
    (void)d.feed(file);
    d.finish();
    return r;
}

ReplayResult replay(std::istream& in, BookBuilder& book, const ReplayOptions& options) {
    ReplayResult r;
    Driver d(book, options, r);
    std::vector<char> buf(options.chunk_bytes == 0 ? 1 : options.chunk_bytes);
    while (in) {
        in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
        const auto n = static_cast<std::size_t>(in.gcount());
        if (n == 0) {
            break;
        }
        const std::span<const std::uint8_t> chunk(reinterpret_cast<const std::uint8_t*>(buf.data()),
                                                  n);
        if (!d.feed(chunk)) {
            break;
        }
    }
    if (in.bad()) {
        r.stopped = "read error";
    }
    d.finish();
    return r;
}

}  // namespace enginex::itch
