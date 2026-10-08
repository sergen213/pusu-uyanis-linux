#version 450
layout(location=0) in vec2 uv;
layout(location=0) out vec4 output_color;
layout(set=0,binding=2) uniform sampler2DArray history_image;
layout(push_constant) uniform QualityPush {
    uint operation,hdr,layer,adapt;
    vec2 direction;float gamma_value,exposure_ev;
    float bloom_strength,alpha;uvec2 padding;
} pc;
vec3 decode_srgb(vec3 c) {
    return mix(c/12.92,pow((c+0.055)/1.055,vec3(2.4)),greaterThan(c,vec3(0.04045)));
}
void main() {
    vec3 c=texture(history_image,vec3(uv,float(pc.layer))).rgb;
    if(pc.hdr!=0u)c=decode_srgb(c);
    output_color=vec4(c,pc.alpha);
}
