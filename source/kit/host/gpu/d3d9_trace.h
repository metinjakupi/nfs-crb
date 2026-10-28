// d3d9_trace.h - opt-in bounded metadata history, owned by the render caller.
// No guest/resource pointers survive a call. Storage is allocated at startup;
// recording neither allocates nor performs I/O. JSON is written only on request.
// This diagnoses submitted work, not game object culling or GPU pixel results.
#pragma once
#include "../../dx/host_d9.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <vector>

namespace d9trace {

inline uint64_t hash(const void *data, size_t bytes) {
    if (!data || !bytes)
        return 0;
    uint64_t h = 14695981039346656037ull;
    const auto *p = static_cast<const uint8_t *>(data);
    for (size_t i = 0; i < bytes; ++i)
        h = (h ^ p[i]) * 1099511628211ull;
    return h;
}

enum class Result : uint32_t {
    Pending,
    Encoded,
    NoPass,
    UndecodedShader,
    UntranslatedShader,
    NoPipeline,
    MissingVertexBuffer,
    MissingIndexBuffer,
    IndexRange,
    UnsupportedPrimitive,
    Empty,
    ShimRejected
};
inline const char *name(Result result) {
    switch (result) {
    case Result::Pending:
        return "pending";
    case Result::Encoded:
        return "encoded";
    case Result::NoPass:
        return "no_pass";
    case Result::UndecodedShader:
        return "undecoded_shader";
    case Result::UntranslatedShader:
        return "untranslated_shader";
    case Result::NoPipeline:
        return "no_pipeline";
    case Result::MissingVertexBuffer:
        return "missing_vertex_buffer";
    case Result::MissingIndexBuffer:
        return "missing_index_buffer";
    case Result::IndexRange:
        return "index_range";
    case Result::UnsupportedPrimitive:
        return "unsupported_primitive";
    case Result::Empty:
        return "empty";
    case Result::ShimRejected:
        return "shim_rejected";
    }
    return "unknown";
}

struct Context {
    uint64_t encoding = 0, submitted = 0, completed = 0, pass = 0;
};
// generation changes on allocation/replacement, revision on CPU upload. GPU
// writes are identified separately; no assertion about completion is implied.
struct Resource {
    uint32_t id = 0, generation = 0, revision = 0;
    uint64_t used = 0, written = 0;
};
constexpr uint32_t state_ids[] = {7,   8,   14,  15,  19,  20,  22,  23,  24,  25,  27,
                                  28,  34,  35,  36,  37,  38,  52,  53,  54,  55,  56,
                                  57,  58,  59,  168, 171, 174, 175, 185, 186, 187, 188,
                                  189, 190, 191, 192, 193, 194, 195, 206, 207, 208, 209};
struct Draw {
    HostD9Admission admission{};
    uint64_t sequence = 0;
    Context context;
    uint64_t vs = 0, ps = 0, declaration = 0, state = 0, samplers = 0;
    uint64_t vertex_constants = 0, pixel_constants = 0, bool_constants = 0;
    uint64_t inline_vertices = 0, inline_indices = 0;
    uint32_t vertex_constant_count = 0, pixel_constant_count = 0, nonfinite_constants = 0;
    uint32_t primitive = 0, count = 0, start = 0, index_size = 0, decl_id = 0;
    int32_t base = 0;
    HostD9Target target{};
    int32_t viewport[4]{}, scissor[4]{};
    uint32_t depth_range_bits[2]{};
    HostD9Stream streams[8]{};
    Resource textures[16]{}, buffers[9]{}, attachments[5]{};
    uint32_t texture_desc[16][4]{}, attachment_desc[5][4]{}; // width,height,D3DFORMAT,kind
    uint32_t render_state[std::size(state_ids)]{};
    uint32_t used_samplers = 0, missing_textures = 0, incompatible_textures = 0;
    uint32_t feedback_textures = 0, fitted_depth = 0, vertex_stream_mask = 0;
    uint32_t samples = 0, vertex_inline_bytes = 0, index_inline_bytes = 0;
    Result result = Result::Pending;
    char label[96]{};
};
struct Event {
    uint64_t sequence = 0;
    Context context;
    Resource resource;
    uint32_t values[16]{};
    char kind[24]{};
};
struct Frame {
    HostD9Accounting accounting{};
    uint64_t number = 0, next_sequence = 0, draw_overflow = 0, event_overflow = 0;
    uint64_t present_wall_us = 0, monotonic_ns = 0;
    uint32_t draws = 0, events = 0;
    uint64_t draws_attempted = 0, events_attempted = 0;
    bool complete = false;
    Context end;
    std::unique_ptr<Draw[]> draw;
    std::unique_ptr<Event[]> event;
    void reset(uint64_t n) {
        accounting = {};
        number = n;
        next_sequence = draw_overflow = event_overflow = draws = events = 0;
        complete = false;
        draws_attempted = events_attempted = 0;
        present_wall_us = monotonic_ns = 0;
        end = {};
    }
};

inline const char *accounting_status(const HostD9Accounting &a) {
    if (!a.enabled)
        return "disabled";
    if (!a.drained)
        return "pending";
    if (a.forwarded != a.received || a.rejected != a.received_rejected ||
        a.forwarded_serial_sum != a.received_serial_sum || a.entries != a.forwarded + a.unforwarded)
        return "unexplained";
    if (a.unforwarded)
        return "unforwarded";
    if (a.entry_serial_sum != a.received_serial_sum || a.last_entry != a.last_received)
        return "unexplained";
    return "complete";
}
inline void accounting_json(FILE *f, const HostD9Accounting &a) {
    if (!a.enabled)
        return;
    fprintf(f,
            ",\"admission_accounting\":{\"status\":\"%s\",\"drained\":%s,"
            "\"entries\":%llu,\"forwarded\":%llu,\"rejected\":%llu,"
            "\"received\":%llu,\"received_rejected\":%llu,\"unforwarded\":%llu,"
            "\"entry_serial_sum\":%llu,\"forwarded_serial_sum\":%llu,\"received_serial_sum\":%llu,"
            "\"last_entry\":%llu,\"last_received\":%llu}",
            accounting_status(a), a.drained ? "true" : "false", (unsigned long long)a.entries,
            (unsigned long long)a.forwarded, (unsigned long long)a.rejected,
            (unsigned long long)a.received, (unsigned long long)a.received_rejected,
            (unsigned long long)a.unforwarded, (unsigned long long)a.entry_serial_sum,
            (unsigned long long)a.forwarded_serial_sum, (unsigned long long)a.received_serial_sum,
            (unsigned long long)a.last_entry, (unsigned long long)a.last_received);
}

inline void quoted(FILE *f, const char *s) {
    fputc('"', f);
    for (; *s; ++s) {
        const unsigned c = static_cast<unsigned char>(*s);
        if (c == '"' || c == '\\')
            fprintf(f, "\\%c", c);
        else if (c < 32 || c >= 127)
            fprintf(f, "\\u%04x", c);
        else
            fputc(static_cast<int>(c), f);
    }
    fputc('"', f);
}
template <class T> inline void numbers(FILE *f, const T *values, size_t count) {
    fputc('[', f);
    for (size_t i = 0; i < count; ++i)
        fprintf(f, "%s%lld", i ? "," : "", static_cast<long long>(values[i]));
    fputc(']', f);
}
inline void context_json(FILE *f, const Context &c) {
    fprintf(f, "{\"encoding\":%llu,\"submitted\":%llu,\"completed\":%llu,\"pass\":%llu}",
            (unsigned long long)c.encoding, (unsigned long long)c.submitted,
            (unsigned long long)c.completed, (unsigned long long)c.pass);
}
inline void resource_json(FILE *f, const Resource &r) {
    fprintf(f, "[%u,%u,%u,%llu,%llu]", r.id, r.generation, r.revision, (unsigned long long)r.used,
            (unsigned long long)r.written);
}
template <size_t N> inline void resources_json(FILE *f, const Resource (&r)[N]) {
    fputc('[', f);
    for (size_t i = 0; i < N; ++i) {
        if (i)
            fputc(',', f);
        resource_json(f, r[i]);
    }
    fputc(']', f);
}
inline void hex_json(FILE *f, const char *key, uint64_t value) {
    fprintf(f, ",\"%s\":\"%016llx\"", key, (unsigned long long)value);
}

class Ring {
  public:
    static constexpr size_t frames = 10;
    explicit Ring(uint32_t draw_capacity = 4096, uint32_t event_capacity = 1024,
                  size_t history = frames)
        : frames_(history + 1), draw_capacity_(draw_capacity), event_capacity_(event_capacity) {
        if (!history || history > 32)
            throw std::invalid_argument("trace history out of range");
        if (!draw_capacity || draw_capacity > 8192 || !event_capacity || event_capacity > 4096)
            throw std::invalid_argument("trace capacity out of range");
        for (auto &f : frames_) {
            f.draw = std::make_unique<Draw[]>(draw_capacity);
            f.event = std::make_unique<Event[]>(event_capacity);
        }
        frames_[0].reset(1);
    }
    size_t allocated_bytes() const {
        return frames_.size() * (draw_capacity_ * sizeof(Draw) + event_capacity_ * sizeof(Event));
    }
    Draw *record(const HostD9Draw &d) {
        Frame &f = frames_[current_];
        const uint64_t seq = f.next_sequence++;
        ++f.draws_attempted;
        if (!retain)
            return nullptr;
        if (f.draws == draw_capacity_) {
            ++f.draw_overflow;
            return nullptr;
        }
        Draw &r = f.draw[f.draws++];
        r = {};
        r.sequence = seq;
        r.admission = d.admission;
        r.vs = d.vs_key;
        r.ps = d.ps_key;
        r.declaration = hash(d.decl, d.decl_size);
        r.decl_id = d.decl_id;
        r.state = hash(d.render_state, 256 * sizeof(uint32_t));
        r.samplers = hash(d.sampler_state, 16 * 14 * sizeof(uint32_t));
        r.vertex_constants = hash(d.vconst, d.vconst_count * 4 * sizeof(float));
        r.pixel_constants = hash(d.pconst, d.pconst_count * 4 * sizeof(float));
        r.bool_constants =
            hash(d.vbool, sizeof d.vbool) * 1099511628211ull ^ hash(d.pbool, sizeof d.pbool);
        r.inline_vertices = hash(d.inline_vertices, d.inline_bytes);
        r.inline_indices = hash(d.inline_indices, d.inline_index_bytes);
        r.vertex_constant_count = d.vconst_count;
        r.pixel_constant_count = d.pconst_count;
        for (uint32_t i = 0; d.vconst && i < d.vconst_count * 4; ++i)
            r.nonfinite_constants += !std::isfinite(d.vconst[i]);
        for (uint32_t i = 0; d.pconst && i < d.pconst_count * 4; ++i)
            r.nonfinite_constants += !std::isfinite(d.pconst[i]);
        r.target = d.target;
        memcpy(r.viewport, d.viewport, sizeof r.viewport);
        memcpy(r.scissor, d.scissor, sizeof r.scissor);
        memcpy(r.depth_range_bits, d.depth_range, sizeof r.depth_range_bits);
        memcpy(r.streams, d.stream, sizeof r.streams);
        for (size_t i = 0; d.render_state && i < std::size(state_ids); ++i)
            r.render_state[i] = d.render_state[state_ids[i]];
        r.primitive = d.primitive;
        r.count = d.primitive_count;
        r.start = d.start;
        r.base = d.base_vertex;
        r.index_size = d.index_size;
        r.vertex_inline_bytes = d.inline_bytes;
        r.index_inline_bytes = d.inline_index_bytes;
        if (d.label)
            snprintf(r.label, sizeof r.label, "%s", d.label);
        return &r;
    }
    Event *event(const char *kind, Context context, Resource resource = {}) {
        Frame &f = frames_[current_];
        const uint64_t seq = f.next_sequence++;
        ++f.events_attempted;
        if (!retain)
            return nullptr;
        if (f.events == event_capacity_) {
            ++f.event_overflow;
            return nullptr;
        }
        Event &e = f.event[f.events++];
        e = {};
        e.sequence = seq;
        e.context = context;
        e.resource = resource;
        snprintf(e.kind, sizeof e.kind, "%s", kind);
        return &e;
    }
    uint64_t current_frame_number() const {
        return frames_[current_].number;
    }
    void accounting(const HostD9Accounting &a) {
        frames_[current_].accounting = a;
    }
    bool retain = true;
    const Frame &current() const {
        return frames_[current_];
    }
    void reset(uint64_t next) {
        for (auto &f : frames_)
            f.reset(0);
        current_ = 0;
        frames_[0].reset(next);
    }
    void present(Context end, uint64_t wall_us = 0, uint64_t mono = 0) {
        Frame &f = frames_[current_];
        f.end = end;
        f.complete = true;
        f.present_wall_us = wall_us;
        f.monotonic_ns = mono;
        const uint64_t next = f.number + 1;
        if (retain)
            current_ = (current_ + 1) % frames_.size();
        frames_[current_].reset(next);
    }
    // Ten most recent frames including a nonempty in-progress frame. One
    // extra slot lets an empty current frame retain ten complete predecessors.
    template <class Fn> void visit(Fn fn) const {
        const size_t frames = frames_.size() - 1;
        const Frame &now = frames_[current_];
        size_t newest = now.next_sequence ? current_ : (current_ + frames) % frames_.size();
        for (size_t age = frames; age > 0; --age) {
            const Frame &f = frames_[(newest + frames_.size() - (age - 1)) % frames_.size()];
            if (f.number && (f.complete || f.next_sequence))
                fn(f);
        }
    }
    bool write(FILE *file, const char *tag, Context at_flush) const {
        if (!file)
            return false;
        fprintf(file, "{\"schema\":1,\"kind\":\"d3d9_metadata_history\",\"tag\":");
        quoted(file, tag ? tag : "");
        fprintf(file,
                ",\"object_ids_available\":false,\"gpu_pixels_captured\":false,"
                "\"resource_fields\":[\"id\",\"generation\",\"cpu_revision\",\"last_used_serial\","
                "\"last_write_serial\"],"
                "\"draw_capacity\":%u,\"event_capacity\":%u,\"flush\":",
                draw_capacity_, event_capacity_);
        context_json(file, at_flush);
        fprintf(file, ",\"render_state_ids\":");
        numbers(file, state_ids, std::size(state_ids));
        fprintf(file, "}\n");
        visit([&](const Frame &f) {
            fprintf(file,
                    "{\"frame\":%llu,\"complete\":%s,\"present_wall_us\":%llu,\"draw_overflow\":%"
                    "llu,\"event_overflow\":%llu,\"end\":",
                    (unsigned long long)f.number, f.complete ? "true" : "false",
                    (unsigned long long)f.present_wall_us, (unsigned long long)f.draw_overflow,
                    (unsigned long long)f.event_overflow);
            context_json(file, f.end);
            accounting_json(file, f.accounting);
            fprintf(file,
                    ",\"monotonic_ns\":%llu,\"draws_attempted\":%llu,\"events_attempted\":%llu,"
                    "\"records_complete\":%s",
                    (unsigned long long)f.monotonic_ns, (unsigned long long)f.draws_attempted,
                    (unsigned long long)f.events_attempted,
                    (!f.draw_overflow && !f.event_overflow) ? "true" : "false");
            fprintf(file, ",\"draws\":[");
            for (uint32_t i = 0; i < f.draws; ++i) {
                const Draw &d = f.draw[i];
                fprintf(file, "%s{\"sequence\":%llu,\"context\":", i ? "," : "",
                        (unsigned long long)d.sequence);
                context_json(file, d.context);
                if (d.admission.serial) {
                    fprintf(file,
                            ",\"admission\":{\"serial\":%llu,\"api\":%u,\"caller\":%u,"
                            "\"device\":%u,\"reason\":%u,\"resource_identity\":",
                            (unsigned long long)d.admission.serial, d.admission.api,
                            d.admission.caller, d.admission.device, d.admission.reason);
                    numbers(file, d.admission.resource_identity, 9);
                    fprintf(file, ",\"inline_guest\":[%u,%u]", d.admission.inline_vertices,
                            d.admission.inline_indices);
                    fprintf(file, "}");
                }
                fprintf(file, ",\"result\":");
                quoted(file, name(d.result));
                fprintf(file, ",\"label\":");
                quoted(file, d.label);
                hex_json(file, "vs", d.vs);
                hex_json(file, "ps", d.ps);
                hex_json(file, "declaration", d.declaration);
                hex_json(file, "state", d.state);
                hex_json(file, "samplers", d.samplers);
                hex_json(file, "vconst", d.vertex_constants);
                hex_json(file, "pconst", d.pixel_constants);
                hex_json(file, "boolconst", d.bool_constants);
                hex_json(file, "inline_vertices", d.inline_vertices);
                hex_json(file, "inline_indices", d.inline_indices);
                fprintf(file,
                        ",\"constant_counts\":[%u,%u],\"nonfinite_constants\":%u,\"decl_id\":%u,"
                        "\"primitive\":%u,\"count\":%u,\"start\":%u,\"base\":%d,\"index_size\":%u,"
                        "\"inline_bytes\":[%u,%u],\"used_samplers\":%u,\"missing_textures\":%u,"
                        "\"incompatible_textures\":%u,\"feedback_textures\":%u,\"fitted_depth\":%u,"
                        "\"stream_mask\":%u,\"samples\":%u,\"viewport\":",
                        d.vertex_constant_count, d.pixel_constant_count, d.nonfinite_constants,
                        d.decl_id, d.primitive, d.count, d.start, d.base, d.index_size,
                        d.vertex_inline_bytes, d.index_inline_bytes, d.used_samplers,
                        d.missing_textures, d.incompatible_textures, d.feedback_textures,
                        d.fitted_depth, d.vertex_stream_mask, d.samples);
                numbers(file, d.viewport, 4);
                fprintf(file, ",\"scissor\":");
                numbers(file, d.scissor, 4);
                fprintf(file, ",\"depth_range_bits\":");
                numbers(file, d.depth_range_bits, 2);
                fprintf(file, ",\"render_state\":");
                numbers(file, d.render_state, std::size(state_ids));
                fprintf(file, ",\"textures\":");
                resources_json(file, d.textures);
                fprintf(file, ",\"buffers\":");
                resources_json(file, d.buffers);
                fprintf(file, ",\"attachments\":");
                resources_json(file, d.attachments);
                fprintf(file, ",\"texture_desc\":[");
                for (size_t j = 0; j < 16; ++j) {
                    if (j)
                        fputc(',', file);
                    numbers(file, d.texture_desc[j], 4);
                }
                fprintf(file, "],\"attachment_desc\":[");
                for (size_t j = 0; j < 5; ++j) {
                    if (j)
                        fputc(',', file);
                    numbers(file, d.attachment_desc[j], 4);
                }
                fputc(']', file);
                fprintf(file, ",\"targets\":[");
                for (size_t j = 0; j < 5; ++j) {
                    const HostD9Surface &s = j == 4 ? d.target.depth : d.target.color[j];
                    fprintf(file, "%s[%u,%u,%u]", j ? "," : "", s.id, s.face, s.level);
                }
                fprintf(file, "],\"streams\":[");
                for (size_t j = 0; j < 8; ++j)
                    fprintf(file, "%s[%u,%u,%u]", j ? "," : "", d.streams[j].buffer,
                            d.streams[j].offset, d.streams[j].stride);
                fprintf(file, "]}");
            }
            fprintf(file, "],\"events\":[");
            for (uint32_t i = 0; i < f.events; ++i) {
                const Event &e = f.event[i];
                fprintf(file, "%s{\"sequence\":%llu,\"kind\":", i ? "," : "",
                        (unsigned long long)e.sequence);
                quoted(file, e.kind);
                fprintf(file, ",\"context\":");
                context_json(file, e.context);
                fprintf(file, ",\"resource\":");
                resource_json(file, e.resource);
                fprintf(file, ",\"values\":");
                numbers(file, e.values, std::size(e.values));
                fprintf(file, "}");
            }
            fprintf(file, "]}\n");
        });
        return !ferror(file);
    }

  private:
    std::vector<Frame> frames_;
    size_t current_ = 0;
    uint32_t draw_capacity_, event_capacity_;
};
} // namespace d9trace
