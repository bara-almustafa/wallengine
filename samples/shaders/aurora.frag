// Aurora borealis over a starry sky. Reacts to bass; mouse moves the sky.
float hash(vec2 p){ return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453); }
float noise(vec2 p){
    vec2 i = floor(p), f = fract(p); f = f * f * (3.0 - 2.0 * f);
    return mix(mix(hash(i), hash(i + vec2(1, 0)), f.x), mix(hash(i + vec2(0, 1)), hash(i + vec2(1, 1)), f.x), f.y);
}
float fbm(vec2 p){ float v = 0.0, a = 0.5; for (int i = 0; i < 5; i++) { v += a * noise(p); p *= 2.03; a *= 0.5; } return v; }

void mainImage(out vec4 fragColor, in vec2 fragCoord)
{
    vec2 uv = fragCoord / iResolution.xy;
    vec2 m = iMouse.xy / iResolution.xy - 0.5;
    vec2 p = (fragCoord - 0.5 * iResolution.xy) / iResolution.y + m * 0.05;

    vec3 col = mix(vec3(0.01, 0.02, 0.06), vec3(0.02, 0.06, 0.12), uv.y);

    // stars
    vec2 sp = p * 120.0;
    float s = hash(floor(sp));
    col += smoothstep(0.995, 1.0, s) * (0.6 + 0.4 * sin(iTime * 3.0 + s * 100.0)) * smoothstep(0.1, 0.5, uv.y);

    // aurora curtains
    for (int i = 0; i < 3; i++) {
        float fi = float(i);
        float x = p.x * (1.2 + fi * 0.3) + iTime * 0.03 * (fi + 1.0);
        float band = fbm(vec2(x * 2.0, iTime * 0.05 + fi * 3.0));
        float y = -0.12 + band * 0.3 + fi * 0.05;
        float d = p.y - y;
        float curtain = exp(-max(d, 0.0) * 6.0) * smoothstep(-0.02, 0.05, d);
        curtain *= 0.5 + 0.5 * fbm(vec2(x * 12.0, p.y * 2.0 - iTime * 0.2));
        vec3 c = mix(vec3(0.1, 1.0, 0.5), vec3(0.5, 0.2, 1.0), fi / 2.0 + d * 1.5);
        col += c * curtain * (0.45 + 0.6 * iBass);
    }

    // silhouetted hills
    float h = -0.32 + 0.06 * fbm(vec2(p.x * 3.0, 1.0)) + 0.03 * sin(p.x * 2.0);
    col = mix(col, vec3(0.005, 0.01, 0.015), smoothstep(0.003, -0.003, p.y - h));

    fragColor = vec4(pow(col, vec3(0.9)), 1.0);
}
