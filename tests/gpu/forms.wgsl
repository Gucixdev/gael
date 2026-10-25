// gpu-each at tests/gpu/forms.gael:10
struct U {
  n: u32,
  k0: f32,
  k1: i32,
}
@group(0) @binding(0) var<uniform> u: U;
@group(0) @binding(1) var<storage, read_write> c0: array<f32>;
@group(0) @binding(2) var<storage, read_write> c1: array<f32>;
@group(0) @binding(3) var<storage, read_write> c2: array<f32>;
@group(0) @binding(4) var<storage, read_write> c3: array<i32>;
@group(0) @binding(5) var<storage, read_write> c4: array<u32>;
@compute @workgroup_size(64)
fn main(@builtin(global_invocation_id) gid: vec3<u32>, @builtin(num_workgroups) nwg: vec3<u32>) {
  let idx = gid.x + gid.y * nwg.x * 64u;
  if (idx >= u.n) {
    return;
  }
  {
    var v_a: f32 = (c0[idx] * u.k0);
    var v_b = min(min(v_a, 3.0), c1[idx]);
    var v_k: i32 = i32(idx);
    c2[idx] = ((max(v_a, v_b) + abs((c1[idx] - 1.0f))) + clamp(c0[idx], 0.5, 2.5));
    if (((v_k > 1i) && (!(c3[idx] == 0i)))) {
      c3[idx] = ((c3[idx] * u.k1) + (v_k % 2i));
    } else {
      c3[idx] = (-c3[idx]);
    }
    if (((v_a < 1.0f) || ((v_k > 3i) != (v_b >= 1.0f)))) {
      c4[idx] = ((c4[idx] << u32(2)) | (u32(v_k) & 1u));
    }
    c4[idx] = (c4[idx] ^ ((~0u) >> u32(28)));
    c1[idx] = select(floor((v_a / 2.0f)), -1.0f, !((v_a > 0.0f) && (v_a < 8.0f)));
    c0[idx] = sqrt((c0[idx] * c0[idx]));
  }
}
