// Used to generate samples/scenes/mountains.png (render with --screenshot).
float hash(vec2 p){ return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453); }
float noise(float x){ float i = floor(x), f = fract(x); f = f * f * (3.0 - 2.0 * f); return mix(hash(vec2(i)), hash(vec2(i + 1.0)), f); }
float ridge(float x, float s){ float v = 0.0, a = 0.5; for (int i = 0; i < 6; i++) { v += a * noise(x * s + s * 10.0); x *= 2.1; a *= 0.5; } return v; }

void mainImage(out vec4 o, in vec2 f)
{
    vec2 uv = f / iResolution.xy;
    vec3 col = mix(vec3(0.98, 0.62, 0.42), vec3(0.18, 0.22, 0.45), pow(uv.y, 0.8));
    vec2 sp = uv - vec2(0.68, 0.48);
    sp.x *= iResolution.x / iResolution.y;
    col += vec3(1.0, 0.8, 0.5) * (smoothstep(0.06, 0.055, length(sp)) + 0.03 / (length(sp) + 0.02) * 0.3);
    for (int i = 0; i < 5; i++) {
        float fi = float(i);
        float h = 0.55 - fi * 0.09 + ridge(uv.x, 3.0 + fi * 2.0) * (0.25 - fi * 0.02);
        vec3 c = mix(vec3(0.35, 0.3, 0.5), vec3(0.04, 0.05, 0.1), fi / 4.0);
        c = mix(c, col, 0.55 - fi * 0.12);
        col = mix(col, c, smoothstep(0.002, -0.002, uv.y - h));
    }
    o = vec4(col, 1.0);
}
