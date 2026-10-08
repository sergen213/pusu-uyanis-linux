#version 450
layout(location=0) in vec2 uv;
layout(location=0) out vec4 output_color;
layout(set=0,binding=0) uniform sampler2D image;
layout(set=0,binding=1) uniform sampler2D bloom_image;
layout(push_constant) uniform QualityPush {
    uint operation,hdr,layer,adapt;
    vec2 direction;float gamma_value,exposure_ev;
    float bloom_strength,alpha;uvec2 padding;
} pc;
vec3 encode_srgb(vec3 c) {
    c=max(c,vec3(0));
    return mix(12.92*c,1.055*pow(c,vec3(1.0/2.4))-0.055,greaterThan(c,vec3(0.0031308)));
}
vec3 decode_srgb(vec3 c) {
    c=max(c,vec3(0));
    return mix(c/12.92,pow((c+0.055)/1.055,vec3(2.4)),greaterThan(c,vec3(0.04045)));
}
void main() {
    vec3 c=texture(image,uv).rgb;
    if(pc.operation==1u) {
        float peak=max(c.r,max(c.g,c.b));
        c*=max(peak-0.8,0.0)/max(peak,0.0001);
    } else if(pc.operation==2u) {
        c*=0.227027;
        c+=texture(image,uv+pc.direction*1.384615).rgb*0.316216;
        c+=texture(image,uv-pc.direction*1.384615).rgb*0.316216;
        c+=texture(image,uv+pc.direction*3.230769).rgb*0.070270;
        c+=texture(image,uv-pc.direction*3.230769).rgb*0.070270;
    } else if(pc.operation==0u) {
        c+=texture(bloom_image,uv).rgb*pc.bloom_strength;
        if(pc.hdr!=0u) {
            c=max(c*exp2(pc.exposure_ev),vec3(0));
            c=encode_srgb(c/(1.0+c));
        }
        // Parity mode intentionally preserves the original byte-domain algebra.
        c=pow(max(c,vec3(0)),vec3(1.0/pc.gamma_value));
    } else if(pc.operation==3u && pc.hdr!=0u) {
        // Original history stores clipped display RGB8, never a tone-mapped frame.
        c=encode_srgb(c);
    } else if(pc.operation==4u && pc.adapt!=0u) {
        // SRGB attachment hardware encodes once; our SDR source is already encoded.
        c=decode_srgb(c);
    }
    output_color=vec4(c,1);
}
