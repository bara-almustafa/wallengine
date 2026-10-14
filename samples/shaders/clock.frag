// Minimal glowing analog clock (uses iDate) on a slow gradient.
#define PI 3.14159265
float seg(vec2 p, vec2 a, vec2 b){ vec2 pa = p - a, ba = b - a; float h = clamp(dot(pa, ba) / dot(ba, ba), 0.0, 1.0); return length(pa - ba * h); }
vec2 hand(float frac, float len){ float a = PI * 0.5 - frac * 2.0 * PI; return vec2(cos(a), sin(a)) * len; }

void mainImage(out vec4 fragColor, in vec2 fragCoord)
{
    vec2 p = (fragCoord - 0.5 * iResolution.xy) / iResolution.y;
    vec3 col = 0.5 + 0.35 * cos(iTime * 0.05 + p.xyx * 1.5 + vec3(0.0, 2.0, 4.0));
    col *= 0.35;

    float secs = iDate.w;
    float s = mod(secs, 60.0), mn = mod(secs / 60.0, 60.0), hr = mod(secs / 3600.0, 12.0);
    float r = length(p);
    float ring = abs(r - 0.3);
    col += vec3(0.9) * smoothstep(0.004, 0.0, ring) + vec3(0.5, 0.7, 1.0) * 0.004 / (ring + 0.004) * 0.3;

    for (int i = 0; i < 12; i++) {
        vec2 d = hand(float(i) / 12.0, 1.0);
        col += smoothstep(0.004, 0.0, seg(p, d * 0.27, d * 0.29));
    }
    col += vec3(1.0) * smoothstep(0.008, 0.0, seg(p, vec2(0), hand(hr / 12.0, 0.15)));
    col += vec3(1.0) * smoothstep(0.005, 0.0, seg(p, vec2(0), hand(mn / 60.0, 0.23)));
    col += vec3(1.0, 0.3, 0.3) * smoothstep(0.003, 0.0, seg(p, vec2(0), hand(floor(s) / 60.0, 0.26)));
    fragColor = vec4(col, 1.0);
}
