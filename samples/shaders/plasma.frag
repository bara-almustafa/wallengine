// Liquid plasma driven by the audio spectrum.
void mainImage(out vec4 fragColor, in vec2 fragCoord)
{
    vec2 p = (fragCoord - 0.5 * iResolution.xy) / iResolution.y * 3.0;
    vec2 m = (iMouse.xy / iResolution.xy - 0.5) * 2.0;
    float t = iTime * 0.4;
    float low = texture(iChannel0, vec2(0.05, 0.25)).x;
    float mid = texture(iChannel0, vec2(0.4, 0.25)).x;

    for (int i = 1; i < 7; i++) {
        float fi = float(i);
        p += vec2(0.7 / fi * sin(fi * p.y + t + 0.3 * fi) + 0.8,
                  0.4 / fi * sin(fi * p.x + t + 0.3 * fi + m.x) - 1.6);
    }
    vec3 col = 0.5 + 0.5 * sin(vec3(p.x, p.y, p.x + p.y) + vec3(0.0, 2.0, 4.0) + low * 2.0);
    col = mix(col * 0.85, col * 1.15, mid);
    fragColor = vec4(col * col, 1.0);
}
