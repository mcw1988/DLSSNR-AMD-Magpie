// AMDNR-ACO: the hook the ACO route lands on -- mochizuki's Vulkan runtime, whose kernels are
// compiled by ACO on RDNA3/RDNA4.
//
// OpenNR runs the network at present, after this chain, so anything this pass computes is work
// the network then has to see through. The only job here is the window-to-fullscreen stretch the
// scaling pipeline needs, done in the most basic way -- a single sample per pixel, no optical
// flow, no guides, nothing else. The optical-flow parameters the XeSS/FSR stubs carried made
// Magpie compute a 4K motion field that nothing consumed once frame generation left the chain;
// leaving them out is the point of this pass.
//
// Named AMDNR-ACO to tell it apart from AMDNR-HIP (the lmxxf route) and from Magpie's own DLSSNR
// slot, which is NVIDIA's neural filter. Keep this pass in the chain even while the network does
// the work: it is the position the network is aimed at, and the slot a frame generator is wired
// to later.
//!MAGPIE EFFECT
//!VERSION 4
//!SORT_NAME AMDNR-ACO

//!PARAMETER
//!LABEL Filter
//!DEFAULT 0
//!OPTION 0 Point (sharpest, the basic stretch)
//!OPTION 1 Linear (smooth)
int filterMode;

//!TEXTURE
Texture2D INPUT;
//!TEXTURE
Texture2D OUTPUT;

//!SAMPLER
//!FILTER POINT
SamplerState smpPoint;

//!SAMPLER
//!FILTER LINEAR
SamplerState smpLinear;

//!PASS 1
//!STYLE PS
//!IN INPUT
//!OUT OUTPUT
MF4 Pass1(float2 pos)
{
    // One sample, the whole pass. Point keeps the network's pixels as they are; Linear is here
    // only for comparing against the older XeSS_SR / FSR*_SR behaviour, which sampled LINEAR
    // everywhere and softened the network's output by one bilinear pass.
    return filterMode == 0 ? INPUT.SampleLevel(smpPoint, pos, 0)
                           : INPUT.SampleLevel(smpLinear, pos, 0);
}
