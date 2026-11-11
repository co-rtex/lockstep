#include "enginex/itch/replay.hpp"

#include <utility>
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
        r_.bytes_read += chunk.size();
        const FrameError e = framer_.feed(chunk, [&](std::span<const std::uint8_t> payload) {
            if (!r_.stopped) {  // the framer finishes its chunk; nothing after a stop counts
                on_payload(payload);
            }
        });
        if (e != FrameError::None) {
            frame_failed(e);
        }
        return !r_.stopped;
    }

    void finish() {
        if (!r_.stopped) {
            if (const FrameError e = framer_.finish(); e != FrameError::None) {
                frame_failed(e);
            }
        }
    }

    void read_failed() {
        if (!r_.stopped) {
            r_.stopped = "read error";
        }
    }

private:
    void on_payload(std::span<const std::uint8_t> payload) {
        ++r_.frames;
        r_.bytes_processed += 2 + payload.size();
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
        if (r_.stopped) {
            return;  // already stopped earlier in this chunk: later bytes are not examined
        }
        r_.frame_error = e;
        r_.frame_error_offset = framer_.error_offset();
        r_.stopped = "framing error " + std::string(to_string(e)) + " at byte offset " +
                     std::to_string(framer_.error_offset());
        r_.stopped_at_message = r_.frames + 1;
    }

    void stop(std::string why) {
        r_.stopped = std::move(why);
        r_.stopped_at_message = r_.frames;
    }

    BookBuilder& book_;
    const ReplayOptions& options_;
    ReplayResult& r_;
    StreamFramer framer_;
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
        d.read_failed();
    }
    d.finish();
    return r;
}

}  // namespace enginex::itch
