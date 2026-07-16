#version 450

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColor;
layout(location = 1) out vec2 outMotion;
layout(location = 2) out float outReactive;
layout(location = 3) out float outTransparency;

layout(push_constant) uniform ScenePush {
    float timeSeconds;
    float previousTimeSeconds;
    float jitterX;
    float jitterY;
    float renderWidth;
    float renderHeight;
    int loadPasses;
    int frameMode;
} pc;

const float kFovY = 1.04719755;
const float kNearZ = 0.1;
const float kFarZ = 1000.0;
const vec3 kEye = vec3(0.0, 2.4, -5.5);
const vec3 kTarget = vec3(0.0, 0.7, 1.4);

mat3 rotationX(float angle) {
    float c = cos(angle);
    float s = sin(angle);
    return mat3(vec3(1.0, 0.0, 0.0), vec3(0.0, c, s), vec3(0.0, -s, c));
}

mat3 rotationY(float angle) {
    float c = cos(angle);
    float s = sin(angle);
    return mat3(vec3(c, 0.0, -s), vec3(0.0, 1.0, 0.0), vec3(s, 0.0, c));
}

float cubeIntersection(vec3 rayOrigin, vec3 rayDirection, out vec3 localHit, out vec3 localNormal) {
    vec3 inverseDirection = 1.0 / rayDirection;
    vec3 first = (-vec3(0.55) - rayOrigin) * inverseDirection;
    vec3 second = (vec3(0.55) - rayOrigin) * inverseDirection;
    vec3 nearAxis = min(first, second);
    vec3 farAxis = max(first, second);
    float nearDistance = max(max(nearAxis.x, nearAxis.y), nearAxis.z);
    float farDistance = min(min(farAxis.x, farAxis.y), farAxis.z);
    if (farDistance < max(nearDistance, 0.0)) {
        return -1.0;
    }

    localHit = rayOrigin + rayDirection * nearDistance;
    vec3 faceDistance = abs(abs(localHit) - vec3(0.55));
    if (faceDistance.x <= faceDistance.y && faceDistance.x <= faceDistance.z) {
        localNormal = vec3(sign(localHit.x), 0.0, 0.0);
    } else if (faceDistance.y <= faceDistance.z) {
        localNormal = vec3(0.0, sign(localHit.y), 0.0);
    } else {
        localNormal = vec3(0.0, 0.0, sign(localHit.z));
    }
    return nearDistance;
}

vec2 projectToUv(vec3 worldPosition, vec3 cameraRight, vec3 cameraUp, vec3 cameraForward) {
    vec3 cameraPosition = worldPosition - kEye;
    float viewZ = max(dot(cameraPosition, cameraForward), kNearZ);
    float tanHalfFov = tan(kFovY * 0.5);
    float aspect = pc.renderWidth / max(pc.renderHeight, 1.0);
    vec2 ndc = vec2(dot(cameraPosition, cameraRight) / (viewZ * tanHalfFov * aspect),
                    dot(cameraPosition, cameraUp) / (viewZ * tanHalfFov));
    return vec2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
}

float depthForWorldPosition(vec3 worldPosition, vec3 cameraForward) {
    float viewZ = max(dot(worldPosition - kEye, cameraForward), kNearZ);
    return clamp(kFarZ / (kFarZ - kNearZ) - (kNearZ * kFarZ) / ((kFarZ - kNearZ) * viewZ),
                 0.0, 1.0);
}

void main() {
    vec3 cameraForward = normalize(kTarget - kEye);
    vec3 cameraRight = normalize(cross(vec3(0.0, 1.0, 0.0), cameraForward));
    vec3 cameraUp = cross(cameraForward, cameraRight);
    vec2 jitterUv = vec2(pc.jitterX / max(pc.renderWidth, 1.0),
                         pc.jitterY / max(pc.renderHeight, 1.0));
    vec2 uv = inUv + jitterUv;
    vec2 ndc = vec2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    float tanHalfFov = tan(kFovY * 0.5);
    float aspect = pc.renderWidth / max(pc.renderHeight, 1.0);
    vec3 rayDirection = normalize(cameraForward + cameraRight * ndc.x * tanHalfFov * aspect +
                                  cameraUp * ndc.y * tanHalfFov);

    vec3 skyTop = vec3(0.16, 0.28, 0.55);
    vec3 horizon = vec3(0.58, 0.66, 0.80);
    vec3 color = mix(skyTop, horizon, clamp(inUv.y, 0.0, 1.0));
    float depth = 1.0;
    vec2 motion = vec2(0.0);
    float reactive = 0.0;

    float floorDistance = -1.0;
    vec3 floorPosition = vec3(0.0);
    if (rayDirection.y < -0.0001) {
        floorDistance = -kEye.y / rayDirection.y;
        floorPosition = kEye + rayDirection * floorDistance;
        if (floorDistance <= 0.0 || abs(floorPosition.x) > 80.0 || floorPosition.z < -24.0 ||
            floorPosition.z > 200.0) {
            floorDistance = -1.0;
        }
    }

    float sweep = 2.6 * sin(pc.timeSeconds * 0.55);
    float bob = 0.85 + 0.18 * sin(pc.timeSeconds * 1.7);
    vec3 cubeCenter = vec3(sweep, bob, 1.4);
    mat3 cubeRotation = rotationY(pc.timeSeconds * 0.9) * rotationX(pc.timeSeconds * 0.7);
    vec3 localRayOrigin = transpose(cubeRotation) * (kEye - cubeCenter);
    vec3 localRayDirection = transpose(cubeRotation) * rayDirection;
    vec3 localHit = vec3(0.0);
    vec3 localNormal = vec3(0.0);
    float cubeDistance = cubeIntersection(localRayOrigin, localRayDirection, localHit, localNormal);

    if (floorDistance > 0.0 && (cubeDistance < 0.0 || floorDistance < cubeDistance)) {
        vec2 cell = floor(floorPosition.xz * 0.5);
        float checker = mod(cell.x + cell.y, 2.0);
        vec3 albedo = mix(vec3(0.16, 0.18, 0.22), vec3(0.32, 0.35, 0.42), checker);
        vec3 lightDirection = normalize(vec3(0.45, 0.8, -0.35));
        vec3 lit = albedo * (0.28 + 0.85 * max(dot(vec3(0.0, 1.0, 0.0), lightDirection), 0.0));
        float viewZ = dot(floorPosition - kEye, cameraForward);
        float fog = clamp((viewZ - 8.0) / 70.0, 0.0, 1.0);
        color = mix(lit, horizon, fog);
        depth = depthForWorldPosition(floorPosition, cameraForward);
    } else if (cubeDistance >= 0.0) {
        vec3 worldHit = cubeRotation * localHit + cubeCenter;
        vec3 worldNormal = normalize(cubeRotation * localNormal);
        vec3 lightDirection = normalize(vec3(0.45, 0.8, -0.35));
        vec3 albedo = vec3(0.95, 0.55, 0.22);
        vec3 lit = albedo * (0.28 + 0.85 * max(dot(worldNormal, lightDirection), 0.0));
        float viewZ = dot(worldHit - kEye, cameraForward);
        float fog = clamp((viewZ - 8.0) / 70.0, 0.0, 1.0);
        color = mix(lit, horizon, fog);
        depth = depthForWorldPosition(worldHit, cameraForward);

        float previousSweep = 2.6 * sin(pc.previousTimeSeconds * 0.55);
        float previousBob = 0.85 + 0.18 * sin(pc.previousTimeSeconds * 1.7);
        vec3 previousCenter = vec3(previousSweep, previousBob, 1.4);
        mat3 previousRotation = rotationY(pc.previousTimeSeconds * 0.9) *
                                rotationX(pc.previousTimeSeconds * 0.7);
        vec3 previousWorldHit = previousRotation * localHit + previousCenter;
        motion = projectToUv(previousWorldHit, cameraRight, cameraUp, cameraForward) -
                 projectToUv(worldHit, cameraRight, cameraUp, cameraForward);
        reactive = 0.15;
    }

    // Configurable ALU work preserves the DX12 app's high-refresh stress purpose without copies.
    vec3 stressed = color;
    int passes = clamp(pc.loadPasses, 0, 512);
    for (int i = 0; i < passes; ++i) {
        float phase = float(i) * 0.013 + dot(stressed, vec3(0.17, 0.31, 0.11));
        stressed += (sin(phase + stressed.zxy * 1.7) - 0.5) * 0.000035;
    }

    outColor = vec4(max(stressed, vec3(0.0)), 1.0);
    outMotion = motion;
    outReactive = reactive;
    outTransparency = 0.0;
    gl_FragDepth = depth;
}
