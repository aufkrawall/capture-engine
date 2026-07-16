#version 450

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColor;

layout(push_constant) uniform UiPush {
    float timeSeconds;
    float fps;
    int mode;
    int suspended;
    int upscaleMode;
    int dlssSupported;
    int fsrSupported;
    int reflexActive;
    float displayWidth;
    float displayHeight;
} pc;

float box(vec2 point, vec2 low, vec2 high) {
    vec2 inside = step(low, point) * (vec2(1.0) - step(high, point));
    return inside.x * inside.y;
}

int rowPattern(int row, int r0, int r1, int r2, int r3, int r4, int r5, int r6) {
    if (row == 0) return r0;
    if (row == 1) return r1;
    if (row == 2) return r2;
    if (row == 3) return r3;
    if (row == 4) return r4;
    if (row == 5) return r5;
    return r6;
}

int glyphRow(int code, int row) {
    if (code == 48) return rowPattern(row, 14, 17, 19, 21, 25, 17, 14);  // 0
    if (code == 49) return rowPattern(row, 4, 12, 4, 4, 4, 4, 14);       // 1
    if (code == 50) return rowPattern(row, 14, 17, 1, 2, 4, 8, 31);      // 2
    if (code == 51) return rowPattern(row, 30, 1, 1, 14, 1, 1, 30);     // 3
    if (code == 58) return rowPattern(row, 0, 4, 4, 0, 4, 4, 0);        // :
    if (code == 65) return rowPattern(row, 14, 17, 17, 31, 17, 17, 17); // A
    if (code == 67) return rowPattern(row, 14, 17, 16, 16, 16, 17, 14); // C
    if (code == 68) return rowPattern(row, 30, 17, 17, 17, 17, 17, 30); // D
    if (code == 69) return rowPattern(row, 31, 16, 16, 30, 16, 16, 31); // E
    if (code == 70) return rowPattern(row, 31, 16, 16, 30, 16, 16, 16); // F
    if (code == 71) return rowPattern(row, 14, 17, 16, 23, 17, 17, 14); // G
    if (code == 72) return rowPattern(row, 17, 17, 17, 31, 17, 17, 17); // H
    if (code == 73) return rowPattern(row, 31, 4, 4, 4, 4, 4, 31);      // I
    if (code == 76) return rowPattern(row, 16, 16, 16, 16, 16, 16, 31); // L
    if (code == 78) return rowPattern(row, 17, 25, 21, 19, 17, 17, 17); // N
    if (code == 79) return rowPattern(row, 14, 17, 17, 17, 17, 17, 14); // O
    if (code == 80) return rowPattern(row, 30, 17, 17, 30, 16, 16, 16); // P
    if (code == 82) return rowPattern(row, 30, 17, 17, 30, 20, 18, 17); // R
    if (code == 83) return rowPattern(row, 15, 16, 16, 14, 1, 1, 30);   // S
    if (code == 84) return rowPattern(row, 31, 4, 4, 4, 4, 4, 4);      // T
    if (code == 85) return rowPattern(row, 17, 17, 17, 17, 17, 17, 14); // U
    if (code == 86) return rowPattern(row, 17, 17, 17, 17, 17, 10, 4);  // V
    return rowPattern(row, 14, 17, 1, 6, 4, 0, 4);
}

int sequenceCode(int sequence, int index) {
    // Status sequences exactly mirror CurrentFGStatusText() in the DX12 switch app.
    if (sequence == 0) { // FG: OFF
        if (index == 0) return 70; if (index == 1) return 71; if (index == 2) return 58;
        if (index == 3) return 32; if (index == 4) return 79; if (index == 5) return 70;
        if (index == 6) return 70; return 0;
    }
    if (sequence == 1) { // FG: DLSS ACTIVE
        const int text[15] = int[](70, 71, 58, 32, 68, 76, 83, 83, 32, 65, 67, 84, 73, 86, 69);
        return index < 15 ? text[index] : 0;
    }
    if (sequence == 2) { // FG: DLSS SUSPENDED
        const int text[18] = int[](70, 71, 58, 32, 68, 76, 83, 83, 32, 83, 85, 83, 80, 69, 78, 68, 69, 68);
        return index < 18 ? text[index] : 0;
    }
    if (sequence == 3) { // FG: FSR ACTIVE
        const int text[14] = int[](70, 71, 58, 32, 70, 83, 82, 32, 65, 67, 84, 73, 86, 69);
        return index < 14 ? text[index] : 0;
    }
    if (sequence == 4) { // FG: FSR SUSPENDED
        const int text[17] = int[](70, 71, 58, 32, 70, 83, 82, 32, 83, 85, 83, 80, 69, 78, 68, 69, 68);
        return index < 17 ? text[index] : 0;
    }
    if (sequence == 5) { // 1 OFF  2 DLSS  3 FSR
        const int text[20] = int[](49, 32, 79, 70, 70, 32, 32, 50, 32, 68, 76, 83, 83, 32, 32, 51, 32, 70, 83, 82);
        return index < 20 ? text[index] : 0;
    }
    const int text[6] = int[](49, 48, 48, 32, 72, 80); // 100 HP
    return index < 6 ? text[index] : 0;
}

float drawGlyph(vec2 point, vec2 origin, float scale, int code) {
    vec2 local = point - origin;
    if (local.x < 0.0 || local.y < 0.0 || local.x >= scale * 5.0 || local.y >= scale * 7.0) {
        return 0.0;
    }
    int column = int(floor(local.x / scale));
    int row = int(floor(local.y / scale));
    int pattern = glyphRow(code, row);
    return (pattern & (1 << (4 - column))) != 0 ? 1.0 : 0.0;
}

float drawText(vec2 point, vec2 origin, float scale, int sequence) {
    float result = 0.0;
    float cursor = 0.0;
    for (int index = 0; index < 20; ++index) {
        int code = sequenceCode(sequence, index);
        if (code == 0) break;
        if (code == 32) {
            cursor += scale * 4.0;
        } else {
            result = max(result, drawGlyph(point, origin + vec2(cursor, 0.0), scale, code));
            cursor += scale * 6.0;
        }
    }
    return result;
}

void main() {
    vec2 pixel = inUv * vec2(pc.displayWidth, pc.displayHeight);
    vec4 color = vec4(0.0);

    vec2 panelLow = vec2(16.0);
    vec2 panelHigh = vec2(min(pc.displayWidth - 16.0, 560.0),
                          min(pc.displayHeight - 16.0, 112.0));
    float panel = panelHigh.x > panelLow.x && panelHigh.y > panelLow.y
                      ? box(pixel, panelLow, panelHigh)
                      : 0.0;
    color = mix(color, vec4(0.015, 0.015, 0.015, 1.0), panel);

    vec3 textColor = vec3(0.86);
    if (pc.mode == 1) {
        textColor = pc.suspended != 0 ? vec3(1.0, 0.36, 0.22) : vec3(0.38, 0.66, 1.0);
    } else if (pc.mode == 2) {
        textColor = pc.suspended != 0 ? vec3(1.0, 0.36, 0.22) : vec3(0.22, 1.0, 0.54);
    }
    int statusSequence = pc.mode == 1 ? (pc.suspended != 0 ? 2 : 1)
                                      : (pc.mode == 2 ? (pc.suspended != 0 ? 4 : 3) : 0);
    float status = drawText(pixel, vec2(30.0, 30.0), 4.0, statusSequence);
    float controls = drawText(pixel, vec2(30.0, 76.0), 3.0, 5);
    color = mix(color, vec4(textColor, 1.0), max(status, controls));

    float hudSweep = sin(pc.timeSeconds * 1.2) * 0.5 + 0.5;
    float hudX = 40.0 + hudSweep * max(pc.displayWidth - 280.0, 0.0);
    float hudY = pc.displayHeight > 220.0 ? pc.displayHeight - 90.0 : 40.0;
    float hudText = drawText(pixel, vec2(hudX, hudY), 5.0, 6);
    color = mix(color, vec4(0.85, 1.0, 0.55, 1.0), hudText);
    float healthFrame = box(pixel, vec2(hudX, hudY + 44.0), vec2(hudX + 232.0, hudY + 60.0));
    float healthFill = box(pixel, vec2(hudX + 3.0, hudY + 47.0),
                           vec2(hudX + 229.0, hudY + 57.0));
    color = mix(color, vec4(0.04, 0.05, 0.06, 1.0), healthFrame);
    color = mix(color, vec4(0.20, 0.95, 0.35, 1.0), healthFill);

    outColor = color;
}
