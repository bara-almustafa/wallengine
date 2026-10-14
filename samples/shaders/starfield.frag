// Warp-speed starfield. Move the mouse to steer, music speeds it up.
float hash(vec2 p){ return fract(sin(dot(p, vec2(12.9898, 78.233))) * 43758.5453); }

void mainImage(out vec4 fragColor, in vec2 fragCoord)
{
    vec2 uv = (fragCoord - 0.5 * iResolution.xy) / iResolution.y;
    vec2 steer = (iMouse.xy / iResolution.xy - 0.5) * 0.4;
    uv -= steer;
    vec3 col = vec3(0.0);
    float speed = 0.15 + iLevel * 0.6;

    for (int layer = 0; layer < 4; layer++) {
        float depth = fract(float(layer) / 4.0 + iTime * speed);
        float scale = mix(20.0, 0.5, depth);
        float fade = depth * smoothstep(1.0, 0.9, depth);
        vec2 gv = uv * scale + float(layer) * 17.0;
        vec2 id = floor(gv);
        vec2 f = fract(gv) - 0.5;
        for (int y = -1; y <= 1; y++)
        for (int x = -1; x <= 1; x++) {
            vec2 o = vec2(x, y);
            float n = hash(id + o);
            vec2 pos = o + vec2(n, fract(n * 34.0)) - 0.5;
            float d = length(f - pos);
            float size = fract(n * 345.32) * 0.6 + 0.2;
            vec3 tint = 0.6 + 0.4 * cos(n * 6.28 + vec3(0.0, 1.0, 2.0));
            col += tint * size * 0.02 / (d + 0.002) * fade * smoothstep(0.5, 0.2, d);
        }
    }
    col += vec3(0.1, 0.05, 0.2) * (1.0 - length(uv));
    fragColor = vec4(col, 1.0);
}
