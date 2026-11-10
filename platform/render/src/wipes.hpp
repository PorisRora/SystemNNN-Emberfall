#pragma once

#include "emberfall/render.hpp"
#include <array>
#include <cstddef>
#include <cstdint>

namespace emberfall::render {
inline constexpr int shard_extent = 1024;

inline constexpr auto default_shard_mesh = [] {
    constexpr std::size_t rays_per_side = 4;
    constexpr std::size_t ray_count = 4 * rays_per_side;
    struct Mesh {
        std::array<Point, 1 + 2 * ray_count> vertices;
        // Each face stores its vertex count followed by the vertex indices.
        std::array<std::array<int, 5>, 2 * ray_count> faces;
    } mesh{};

    constexpr Point impact{455, 443};
    constexpr std::array<Point, 4> corners{{{0, 0}, {0, shard_extent},
                                           {shard_extent, shard_extent}, {shard_extent, 0}}};
    mesh.vertices[0] = impact;
    const auto along = [](Point from, Point to, int percent) {
        return Point{from.x + (to.x - from.x) * percent / 100,
                     from.y + (to.y - from.y) * percent / 100};
    };
    // Give each ray an uneven perimeter position and an uneven split along its length.
    std::uint32_t seed = 0x454d4246U;
    const auto jitter = [&seed](int range) {
        seed = seed * 1664525U + 1013904223U;
        return static_cast<int>((seed >> 16) % static_cast<unsigned>(range));
    };
    for (std::size_t i = 0; i < ray_count; ++i) {
        const auto side = i / rays_per_side;
        const auto segment = i % rays_per_side;
        const int percent = segment == 0 ? 0 : static_cast<int>(segment * 100 / rays_per_side) + jitter(17) - 8;
        const auto edge = along(corners[side], corners[(side + 1) % corners.size()], percent);
        mesh.vertices[1 + i] = along(impact, edge, 38 + jitter(29));
        mesh.vertices[1 + ray_count + i] = edge;
    }

    // The scanline renderers follow the two edges down from the top vertex.
    const auto top_first = [&mesh](const std::array<int, 5>& face) {
        const auto count = static_cast<std::size_t>(face[0]);
        std::size_t top = 0;
        for (std::size_t i = 1; i < count; ++i) {
            const auto p = mesh.vertices[static_cast<std::size_t>(face[i + 1])];
            const auto q = mesh.vertices[static_cast<std::size_t>(face[top + 1])];
            if (p.y < q.y || (p.y == q.y && p.x < q.x)) top = i;
        }
        std::array<int, 5> ordered{face[0]};
        for (std::size_t i = 0; i < count; ++i) ordered[i + 1] = face[1 + (top + i) % count];
        return ordered;
    };
    constexpr auto outer = static_cast<int>(ray_count);
    for (std::size_t i = 0; i < ray_count; ++i) {
        const int inner = 1 + static_cast<int>(i);
        const int next = 1 + static_cast<int>((i + 1) % ray_count);
        mesh.faces[i * 2] = top_first({3, 0, inner, next});
        mesh.faces[i * 2 + 1] = top_first({4, inner, inner + outer, next + outer, next});
    }
    return mesh;
}();

inline Point scale_shard_vertex(Point vertex, int width, int height) {
    return {vertex.x * (width - 1) / shard_extent, vertex.y * (height - 1) / shard_extent};
}

}
