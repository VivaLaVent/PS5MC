#if (defined(KODI_TONE_MAPPING_REINHARD) || defined(KODI_TONE_MAPPING_ACES) || defined(KODI_TONE_MAPPING_HABLE) || defined(KODI_HLG_TO_PQ))
const float ST2084_m1 = 2610.0 / (4096.0 * 4.0);
const float ST2084_m2 = (2523.0 / 4096.0) * 128.0;
const float ST2084_c1 = 3424.0 / 4096.0;
const float ST2084_c2 = (2413.0 / 4096.0) * 32.0;
const float ST2084_c3 = (2392.0 / 4096.0) * 32.0;
#endif

#if defined(KODI_TONE_MAPPING_REINHARD)
float reinhard(float x)
{
  return x * (1.0 + x / (m_toneP1 * m_toneP1)) / (1.0 + x);
}
#endif

#if defined(KODI_TONE_MAPPING_ACES)
vec3 aces(vec3 x)
{
  float A = 2.51;
  float B = 0.03;
  float C = 2.43;
  float D = 0.59;
  float E = 0.14;
  return (x * (A * x + B)) / (x * (C * x + D) + E);
}
#endif

#if defined(KODI_TONE_MAPPING_HABLE)
vec3 hable(vec3 x)
{
  float A = 0.15;
  float B = 0.5;
  float C = 0.1;
  float D = 0.2;
  float E = 0.02;
  float F = 0.3;
  return ((x * (A * x + C * B) + D * E) / (x * (A * x + B) + D * F)) - E / F;
}
#endif

#if (defined(KODI_TONE_MAPPING_ACES) || defined(KODI_TONE_MAPPING_HABLE))
vec3 inversePQ(vec3 x)
{
  x = pow(max(x, 0.0), vec3(1.0 / ST2084_m2));
  x = max(x - ST2084_c1, 0.0) / (ST2084_c2 - ST2084_c3 * x);
  x = pow(x, vec3(1.0 / ST2084_m1));
  return x;
}
#endif

#if defined(KODI_HLG_TO_PQ)
// HLG video on a PQ-only HDR output (ITU-R BT.2100): HLG signal -> scene light
// (inverse OETF) -> display light for a 1000 cd/m2 display (OOTF, gamma 1.2)
// -> PQ signal, normalised to 10000 cd/m2.
const float HLG_a = 0.17883277;
const float HLG_b = 0.28466892;
const float HLG_c = 0.55991073;
vec3 hlgToPq(vec3 e)
{
  e = clamp(e, 0.0, 1.0);
  vec3 low = e * e / 3.0;
  vec3 high = (exp((e - HLG_c) / HLG_a) + HLG_b) / 12.0;
  vec3 scene = mix(low, high, step(0.5, e));
  float ys = dot(scene, vec3(0.2627, 0.6780, 0.0593));
  vec3 display = scene * pow(max(ys, 1e-6), 0.2) * (1000.0 / 10000.0);
  vec3 p = pow(max(display, 0.0), vec3(ST2084_m1));
  return pow((ST2084_c1 + ST2084_c2 * p) / (1.0 + ST2084_c3 * p), vec3(ST2084_m2));
}
#endif
