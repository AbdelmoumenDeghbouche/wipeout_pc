// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#version 450

layout (location = 0) in vec2 uv;
layout (location = 0) out vec4 color;

layout (binding = 0) uniform sampler2D texSampler;

layout (push_constant) uniform settings {
    float gamma;
    bool hdr;
    bool srgb_input;
    vec4 picture; // gow3: x0, y0, x1, y1 of the part shown; the rest is black (cutscene bars)
    float hdr_scale; // gow3: HDR brightness (1 = the game's)
    float hdr_peak;  // gow3: nits the highlights roll off to (10000 = no limit)
} pp;

// SMPTE ST 2084 (PQ), the HDR output's encoding: code value <-> nits.
const float m1 = 0.1593017578125, m2 = 78.84375, c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
vec3 pq_to_nits(vec3 e) {
    vec3 p = pow(clamp(e, 0.0, 1.0), vec3(1.0 / m2));
    return 10000.0 * pow(max(p - c1, 0.0) / (c2 - c3 * p), vec3(1.0 / m1));
}
vec3 nits_to_pq(vec3 n) {
    vec3 y = pow(clamp(n / 10000.0, 0.0, 1.0), vec3(m1));
    return pow((c1 + c2 * y) / (1.0 + c3 * y), vec3(m2));
}

const float cutoff = 0.0031308, a = 1.055, b = 0.055, d = 12.92;
vec3 gamma(vec3 rgb) {
    return mix(
        a * pow(rgb, vec3(1.0 / (2.4 + 1.0 - pp.gamma))) - b,
        d * rgb / pp.gamma,
        lessThan(rgb, vec3(cutoff))
    );
}

// Exact inverse of gamma() at unit gamma, for buffers that are sRGB encoded but must be sampled
// through a UNORM view because Vulkan has no sRGB variant of the 10-bit format.
vec3 degamma(vec3 rgb) {
    return mix(
        pow(max(rgb + b, 0.0) / a, vec3(2.4)),
        rgb / d,
        lessThan(rgb, vec3(d * cutoff))
    );
}

void main() {
    if (any(lessThan(uv, pp.picture.xy)) || any(greaterThan(uv, pp.picture.zw))) {
        color = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }
    vec4 color_linear = texture(texSampler, uv);
    if (pp.hdr) {
        color = color_linear;
        if (pp.hdr_scale != 1.0 || pp.hdr_peak < 10000.0) {
            // gow3: in-game HDR calibration. Scale, then roll the brightest channel off from half
            // the peak towards the peak (hue kept), so highlights keep detail on a dimmer display.
            vec3 n = pq_to_nits(color.rgb) * pp.hdr_scale;
            float m = max(max(n.r, n.g), n.b), knee = 0.5 * pp.hdr_peak, room = pp.hdr_peak - knee;
            if (m > knee) {
                n *= (knee + room * (1.0 - exp(-(m - knee) / room))) / m;
            }
            color.rgb = nits_to_pq(n);
        }
    } else {
        if (pp.srgb_input) color_linear.rgb = degamma(color_linear.rgb);
        color = vec4(gamma(color_linear.rgb), color_linear.a);
    }
}
