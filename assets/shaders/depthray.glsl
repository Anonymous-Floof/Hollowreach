// World-position reconstruction from the depth buffer.
//
// Deliberately NOT an inverse view-projection unproject. Perspective depth bunches
// up near 1.0, and a full-matrix unproject in float32 then loses several blocks of
// precision in the near and mid field — which visibly corrupted the shadow and fog
// lookups. Isolating the non-linearity into one well-conditioned division keeps the
// reconstruction accurate at every distance.
//
// Expects the camera basis uniforms declared by whichever pass includes this.

#ifndef HR_DEPTHRAY_GLSL
#define HR_DEPTHRAY_GLSL

vec3 worldFromDepth(vec2 uv, float d) {
  vec2 ndc = uv * 2.0 - 1.0;
  // A ray whose forward component is exactly 1, so scaling it by the view-forward
  // distance lands on the surface.
  vec3 ray = uCamFwd + ndc.x * uTanHalf * uAspect * uCamRight + ndc.y * uTanHalf * uCamUp;
  float zc = d * 2.0 - 1.0;
  float linDist = (2.0 * uNear * uFar) / (uFar + uNear - zc * (uFar - uNear));
  return uCamPos + ray * linDist;
}

#endif
