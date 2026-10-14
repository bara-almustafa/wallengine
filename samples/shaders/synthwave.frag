// Retro synthwave sun and neon grid. The sun and grid pulse with the music.
float spec(float x){ return texture(iChannel0, vec2(x, 0.25)).x; }

void mainImage(out vec4 fragColor, in vec2 fragCoord)
{
    vec2 p = (fragCoord - 0.5 * iResolution.xy) / iResolution.y;
    p.x += (iMouse.x / iResolution.x - 0.5) * 0.05;
    float horizon = -0.05;

    vec3 sky = mix(vec3(0.95, 0.3, 0.55), vec3(0.08, 0.02, 0.2), smoothstep(-0.05, 0.5, p.y));
    vec3 col = sky;

    // sun with scanline cut-outs
    vec2 sc = p - vec2(0.0, 0.17);
    float r = 0.26 + iBass * 0.02;
    float sun = smoothstep(r, r - 0.005, length(sc));
    float lines = step(0.5, fract((sc.y + 0.3) * 18.0 - iTime * 0.4)) + step(0.0, sc.y + 0.02);
    sun *= clamp(lines, 0.0, 1.0);
    vec3 sunc = mix(vec3(1.0, 0.15, 0.5), vec3(1.0, 0.85, 0.2), smoothstep(-0.2, 0.25, sc.y));
    col = mix(col, sunc, sun);
    col += vec3(1.0, 0.3, 0.6) * 0.08 / (length(sc) + 0.05) * (0.4 + iBass);

    // spectrum skyline
    float bx = floor((p.x + 1.0) * 40.0) / 80.0;
    float b = spec(abs(bx - 0.5) * 1.6) * 0.18;
    if (p.y > horizon && p.y < horizon + b) col = mix(col, vec3(0.1, 0.0, 0.2), 0.85);

    // grid floor
    if (p.y < horizon) {
        float z = 0.35 / (horizon - p.y);
        vec2 g = vec2(p.x * z, z + iTime * 2.0);
        vec2 gl = abs(fract(g) - 0.5) / fwidth(g);
        float line = 1.0 - min(min(gl.x, gl.y), 1.0);
        vec3 floorc = vec3(0.03, 0.0, 0.07);
        vec3 neon = mix(vec3(1.0, 0.1, 0.8), vec3(0.1, 0.9, 1.0), iBass);
        col = floorc + neon * line * exp(-z * 0.08) * 1.3;
        col += vec3(1.0, 0.2, 0.6) * 0.3 * exp(-(horizon - p.y) * 14.0);
    }
    fragColor = vec4(col, 1.0);
}
