#include <jni.h>
#include <android/asset_manager.h>
#include <android/asset_manager_jni.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <android/log.h>
#include <vulkan/vulkan.h>
#include <vector>
#include <string>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <algorithm>

#define LOG_TAG "CameraVulkan"
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define VK_CHECK(expr) do { VkResult _r=(expr); if (_r != VK_SUCCESS) { LOGE("Vulkan error %d at %s:%d", (int)_r, __FILE__, __LINE__); throw std::runtime_error(std::string("Vulkan error ") + std::to_string((int)_r)); } } while(0)

#include <stdexcept>
struct PushParams { float angle, offsetX, offsetY, crop, rotation, invWidth, invHeight; };
struct VulkanRenderer {
    ANativeWindow* window = nullptr;
    int inW=0, inH=0, outW=0, outH=0;
    VkInstance instance=VK_NULL_HANDLE; VkSurfaceKHR surface=VK_NULL_HANDLE;
    VkPhysicalDevice physical=VK_NULL_HANDLE; VkDevice device=VK_NULL_HANDLE;
    uint32_t queueFamily=0; VkQueue queue=VK_NULL_HANDLE;
    VkSwapchainKHR swapchain=VK_NULL_HANDLE; VkFormat swapFormat=VK_FORMAT_UNDEFINED; VkExtent2D extent{};
    std::vector<VkImage> swapImages; std::vector<VkImageView> swapViews; std::vector<VkFramebuffer> framebuffers;
    VkRenderPass renderPass=VK_NULL_HANDLE; VkPipelineLayout pipelineLayout=VK_NULL_HANDLE; VkPipeline pipeline=VK_NULL_HANDLE;
    VkDescriptorSetLayout descriptorLayout=VK_NULL_HANDLE; VkDescriptorPool descriptorPool=VK_NULL_HANDLE; VkDescriptorSet descriptorSet=VK_NULL_HANDLE;
    VkSampler sampler=VK_NULL_HANDLE; VkImage inputImages[3]{}; VkDeviceMemory inputMemories[3]{}; VkImageView inputViews[3]{};
    VkBuffer stagingBuffers[3]{}; VkDeviceMemory stagingMemories[3]{}; void* stagingMaps[3]{}; VkImageLayout inputLayouts[3]={VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_UNDEFINED};
    VkCommandPool commandPool=VK_NULL_HANDLE; VkCommandBuffer command=VK_NULL_HANDLE;
    VkSemaphore imageAvailable=VK_NULL_HANDLE; std::vector<VkSemaphore> renderFinishedSemaphores; VkFence fence=VK_NULL_HANDLE;
    VkImageLayout inputLayout=VK_IMAGE_LAYOUT_UNDEFINED;

    uint32_t memoryType(uint32_t bits, VkMemoryPropertyFlags props) {
        VkPhysicalDeviceMemoryProperties mp{}; vkGetPhysicalDeviceMemoryProperties(physical,&mp);
        for(uint32_t i=0;i<mp.memoryTypeCount;i++) if((bits&(1u<<i)) && (mp.memoryTypes[i].propertyFlags&props)==props) return i;
        throw std::runtime_error("Vulkan memory type tidak tersedia");
    }
    void createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags props, VkBuffer& buf, VkDeviceMemory& mem) {
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; bi.size=size; bi.usage=usage; bi.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
        VK_CHECK(vkCreateBuffer(device,&bi,nullptr,&buf)); VkMemoryRequirements req{}; vkGetBufferMemoryRequirements(device,buf,&req);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; ai.allocationSize=req.size; ai.memoryTypeIndex=memoryType(req.memoryTypeBits,props);
        VK_CHECK(vkAllocateMemory(device,&ai,nullptr,&mem)); VK_CHECK(vkBindBufferMemory(device,buf,mem,0));
    }
    void createImage(uint32_t w,uint32_t h,VkFormat fmt,VkImageUsageFlags usage,VkImage& img,VkDeviceMemory& mem) {
        VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO}; ii.imageType=VK_IMAGE_TYPE_2D; ii.format=fmt; ii.extent={w,h,1}; ii.mipLevels=1; ii.arrayLayers=1; ii.samples=VK_SAMPLE_COUNT_1_BIT; ii.tiling=VK_IMAGE_TILING_OPTIMAL; ii.usage=usage; ii.sharingMode=VK_SHARING_MODE_EXCLUSIVE; ii.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;
        VK_CHECK(vkCreateImage(device,&ii,nullptr,&img)); VkMemoryRequirements req{}; vkGetImageMemoryRequirements(device,img,&req);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; ai.allocationSize=req.size; ai.memoryTypeIndex=memoryType(req.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        VK_CHECK(vkAllocateMemory(device,&ai,nullptr,&mem)); VK_CHECK(vkBindImageMemory(device,img,mem,0));
    }
    VkImageView makeView(VkImage img,VkFormat fmt) {
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; vi.image=img; vi.viewType=VK_IMAGE_VIEW_TYPE_2D; vi.format=fmt; vi.subresourceRange.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT; vi.subresourceRange.levelCount=1; vi.subresourceRange.layerCount=1;
        VkImageView v; VK_CHECK(vkCreateImageView(device,&vi,nullptr,&v)); return v;
    }
    std::vector<uint32_t> readShader(AAssetManager* mgr,const char* name) {
        AAsset* a=AAssetManager_open(mgr,name,AASSET_MODE_BUFFER); if(!a) throw std::runtime_error(std::string("Shader Vulkan hilang: ")+name);
        off_t len=AAsset_getLength(a); if(len<=0 || (len%4)!=0){AAsset_close(a);throw std::runtime_error("Shader SPIR-V tidak valid");}
        std::vector<uint32_t> b((size_t)len/4); int got=AAsset_read(a,b.data(),(size_t)len); AAsset_close(a); if(got!=len) throw std::runtime_error("Gagal membaca shader SPIR-V"); return b;
    }
    void init(AAssetManager* assets, jobject javaSurface, JNIEnv* env) {
        window=ANativeWindow_fromSurface(env,javaSurface); if(!window) throw std::runtime_error("Encoder Surface tidak tersedia untuk Vulkan");
        ANativeWindow_setBuffersGeometry(window,outW,outH,WINDOW_FORMAT_RGBA_8888);
        const char* instExts[]={VK_KHR_SURFACE_EXTENSION_NAME,VK_KHR_ANDROID_SURFACE_EXTENSION_NAME};
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO}; app.pApplicationName="Camera"; app.apiVersion=VK_API_VERSION_1_0;
        VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; ici.pApplicationInfo=&app; ici.enabledExtensionCount=2; ici.ppEnabledExtensionNames=instExts;
        VK_CHECK(vkCreateInstance(&ici,nullptr,&instance));
        VkAndroidSurfaceCreateInfoKHR sci{VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR}; sci.window=window; VK_CHECK(vkCreateAndroidSurfaceKHR(instance,&sci,nullptr,&surface));
        uint32_t n=0; VK_CHECK(vkEnumeratePhysicalDevices(instance,&n,nullptr)); if(!n) throw std::runtime_error("GPU tidak menyediakan Vulkan"); std::vector<VkPhysicalDevice> devices(n); VK_CHECK(vkEnumeratePhysicalDevices(instance,&n,devices.data()));
        for(auto d:devices){uint32_t qn=0;vkGetPhysicalDeviceQueueFamilyProperties(d,&qn,nullptr);std::vector<VkQueueFamilyProperties> qs(qn);vkGetPhysicalDeviceQueueFamilyProperties(d,&qn,qs.data());for(uint32_t q=0;q<qn;q++){VkBool32 present=VK_FALSE;vkGetPhysicalDeviceSurfaceSupportKHR(d,q,surface,&present);if((qs[q].queueFlags&VK_QUEUE_GRAPHICS_BIT)&&present){physical=d;queueFamily=q;break;}}if(physical)break;}
        if(!physical) throw std::runtime_error("Tidak ada queue Vulkan yang mendukung render dan present ke encoder");
        float priority=1.f; VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};qci.queueFamilyIndex=queueFamily;qci.queueCount=1;qci.pQueuePriorities=&priority;
        const char* devExts[]={VK_KHR_SWAPCHAIN_EXTENSION_NAME};VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};dci.queueCreateInfoCount=1;dci.pQueueCreateInfos=&qci;dci.enabledExtensionCount=1;dci.ppEnabledExtensionNames=devExts;VK_CHECK(vkCreateDevice(physical,&dci,nullptr,&device));vkGetDeviceQueue(device,queueFamily,0,&queue);
        VkSurfaceCapabilitiesKHR caps{};VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical,surface,&caps));uint32_t fn=0;VK_CHECK(vkGetPhysicalDeviceSurfaceFormatsKHR(physical,surface,&fn,nullptr));if(!fn)throw std::runtime_error("Format surface Vulkan tidak tersedia");std::vector<VkSurfaceFormatKHR> formats(fn);VK_CHECK(vkGetPhysicalDeviceSurfaceFormatsKHR(physical,surface,&fn,formats.data()));
        VkSurfaceFormatKHR chosen=formats[0];for(auto f:formats){if(f.format==VK_FORMAT_R8G8B8A8_UNORM || f.format==VK_FORMAT_B8G8R8A8_UNORM){chosen=f;break;}}swapFormat=chosen.format;
        extent=caps.currentExtent;if(extent.width==UINT32_MAX){extent.width=std::clamp((uint32_t)outW,caps.minImageExtent.width,caps.maxImageExtent.width);extent.height=std::clamp((uint32_t)outH,caps.minImageExtent.height,caps.maxImageExtent.height);}if(extent.width!= (uint32_t)outW || extent.height!=(uint32_t)outH) LOGE("Vulkan extent %ux%u differs requested %dx%d",extent.width,extent.height,outW,outH);
        uint32_t imageCount=std::max(caps.minImageCount,2u);if(caps.maxImageCount && imageCount>caps.maxImageCount)imageCount=caps.maxImageCount;
        VkSwapchainCreateInfoKHR sw{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};sw.surface=surface;sw.minImageCount=imageCount;sw.imageFormat=swapFormat;sw.imageColorSpace=chosen.colorSpace;sw.imageExtent=extent;sw.imageArrayLayers=1;sw.imageUsage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;sw.imageSharingMode=VK_SHARING_MODE_EXCLUSIVE;sw.preTransform=caps.currentTransform;VkCompositeAlphaFlagBitsKHR alpha=VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;const VkCompositeAlphaFlagBitsKHR alphaOptions[]={VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR};for(auto a:alphaOptions){if(caps.supportedCompositeAlpha&a){alpha=a;break;}}sw.compositeAlpha=alpha;sw.presentMode=VK_PRESENT_MODE_FIFO_KHR;sw.clipped=VK_TRUE;VK_CHECK(vkCreateSwapchainKHR(device,&sw,nullptr,&swapchain));
        uint32_t scn=0;VK_CHECK(vkGetSwapchainImagesKHR(device,swapchain,&scn,nullptr));swapImages.resize(scn);VK_CHECK(vkGetSwapchainImagesKHR(device,swapchain,&scn,swapImages.data()));for(auto im:swapImages)swapViews.push_back(makeView(im,swapFormat));
        VkAttachmentDescription att{};att.format=swapFormat;att.samples=VK_SAMPLE_COUNT_1_BIT;att.loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR;att.storeOp=VK_ATTACHMENT_STORE_OP_STORE;att.stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE;att.stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE;att.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;att.finalLayout=VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        VkAttachmentReference ar{0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};VkSubpassDescription sub{};sub.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS;sub.colorAttachmentCount=1;sub.pColorAttachments=&ar;VkSubpassDependency dep{};dep.srcSubpass=VK_SUBPASS_EXTERNAL;dep.dstSubpass=0;dep.srcStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;dep.dstStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;dep.dstAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};rp.attachmentCount=1;rp.pAttachments=&att;rp.subpassCount=1;rp.pSubpasses=&sub;rp.dependencyCount=1;rp.pDependencies=&dep;VK_CHECK(vkCreateRenderPass(device,&rp,nullptr,&renderPass));
        for(auto view:swapViews){VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};fi.renderPass=renderPass;fi.attachmentCount=1;fi.pAttachments=&view;fi.width=extent.width;fi.height=extent.height;fi.layers=1;VkFramebuffer fb;VK_CHECK(vkCreateFramebuffer(device,&fi,nullptr,&fb));framebuffers.push_back(fb);}
        const uint32_t planeW[3]={(uint32_t)inW,(uint32_t)inW/2,(uint32_t)inW/2};
        const uint32_t planeH[3]={(uint32_t)inH,(uint32_t)inH/2,(uint32_t)inH/2};
        const VkFormat planeFmt[3]={VK_FORMAT_R8_UNORM,VK_FORMAT_R8_UNORM,VK_FORMAT_R8_UNORM};
        for(int i=0;i<3;i++){createImage(planeW[i],planeH[i],planeFmt[i],VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_SAMPLED_BIT,inputImages[i],inputMemories[i]);inputViews[i]=makeView(inputImages[i],planeFmt[i]);createBuffer((VkDeviceSize)planeW[i]*planeH[i],VK_BUFFER_USAGE_TRANSFER_SRC_BIT,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,stagingBuffers[i],stagingMemories[i]);VK_CHECK(vkMapMemory(device,stagingMemories[i],0,VK_WHOLE_SIZE,0,&stagingMaps[i]));}
        VkSamplerCreateInfo sm{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};sm.magFilter=VK_FILTER_LINEAR;sm.minFilter=VK_FILTER_LINEAR;sm.addressModeU=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;sm.addressModeV=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;sm.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;sm.maxLod=0;VK_CHECK(vkCreateSampler(device,&sm,nullptr,&sampler));
        VkDescriptorSetLayoutBinding binds[3]{};for(uint32_t i=0;i<3;i++){binds[i].binding=i;binds[i].descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;binds[i].descriptorCount=1;binds[i].stageFlags=VK_SHADER_STAGE_FRAGMENT_BIT;}VkDescriptorSetLayoutCreateInfo dl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};dl.bindingCount=3;dl.pBindings=binds;VK_CHECK(vkCreateDescriptorSetLayout(device,&dl,nullptr,&descriptorLayout));
        VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,3};VkDescriptorPoolCreateInfo dp{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};dp.maxSets=1;dp.poolSizeCount=1;dp.pPoolSizes=&ps;VK_CHECK(vkCreateDescriptorPool(device,&dp,nullptr,&descriptorPool));VkDescriptorSetAllocateInfo da{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};da.descriptorPool=descriptorPool;da.descriptorSetCount=1;da.pSetLayouts=&descriptorLayout;VK_CHECK(vkAllocateDescriptorSets(device,&da,&descriptorSet));VkDescriptorImageInfo dis[3]{};VkWriteDescriptorSet wrs[3]{};for(uint32_t i=0;i<3;i++){dis[i]={sampler,inputViews[i],VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};wrs[i]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};wrs[i].dstSet=descriptorSet;wrs[i].dstBinding=i;wrs[i].descriptorCount=1;wrs[i].descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;wrs[i].pImageInfo=&dis[i];}vkUpdateDescriptorSets(device,3,wrs,0,nullptr);
        VkPushConstantRange pcr{};pcr.stageFlags=VK_SHADER_STAGE_FRAGMENT_BIT;pcr.offset=0;pcr.size=sizeof(PushParams);VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};pl.setLayoutCount=1;pl.pSetLayouts=&descriptorLayout;pl.pushConstantRangeCount=1;pl.pPushConstantRanges=&pcr;VK_CHECK(vkCreatePipelineLayout(device,&pl,nullptr,&pipelineLayout));
        auto vs=readShader(assets,"shaders/fullscreen.vert.spv");auto fs=readShader(assets,"shaders/stabilize.frag.spv");VkShaderModuleCreateInfo smi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};smi.codeSize=vs.size()*4;smi.pCode=vs.data();VkShaderModule vsm;VK_CHECK(vkCreateShaderModule(device,&smi,nullptr,&vsm));smi.codeSize=fs.size()*4;smi.pCode=fs.data();VkShaderModule fsm;VK_CHECK(vkCreateShaderModule(device,&smi,nullptr,&fsm));
        VkPipelineShaderStageCreateInfo stages[2]{};stages[0]={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};stages[0].stage=VK_SHADER_STAGE_VERTEX_BIT;stages[0].module=vsm;stages[0].pName="main";stages[1]={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};stages[1].stage=VK_SHADER_STAGE_FRAGMENT_BIT;stages[1].module=fsm;stages[1].pName="main";
        VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};ia.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};vp.viewportCount=1;vp.scissorCount=1;VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};rs.polygonMode=VK_POLYGON_MODE_FILL;rs.cullMode=VK_CULL_MODE_NONE;rs.frontFace=VK_FRONT_FACE_COUNTER_CLOCKWISE;rs.lineWidth=1.f;VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};ms.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT;VkPipelineColorBlendAttachmentState cba{};cba.colorWriteMask=VK_COLOR_COMPONENT_R_BIT|VK_COLOR_COMPONENT_G_BIT|VK_COLOR_COMPONENT_B_BIT|VK_COLOR_COMPONENT_A_BIT;VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};cb.attachmentCount=1;cb.pAttachments=&cba;VkDynamicState dyns[]={VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR};VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};dy.dynamicStateCount=2;dy.pDynamicStates=dyns;
        VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};gp.stageCount=2;gp.pStages=stages;gp.pVertexInputState=&vi;gp.pInputAssemblyState=&ia;gp.pViewportState=&vp;gp.pRasterizationState=&rs;gp.pMultisampleState=&ms;gp.pColorBlendState=&cb;gp.pDynamicState=&dy;gp.layout=pipelineLayout;gp.renderPass=renderPass;gp.subpass=0;VK_CHECK(vkCreateGraphicsPipelines(device,VK_NULL_HANDLE,1,&gp,nullptr,&pipeline));vkDestroyShaderModule(device,vsm,nullptr);vkDestroyShaderModule(device,fsm,nullptr);
        VkCommandPoolCreateInfo cp{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};cp.queueFamilyIndex=queueFamily;cp.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;VK_CHECK(vkCreateCommandPool(device,&cp,nullptr,&commandPool));VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};ca.commandPool=commandPool;ca.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;ca.commandBufferCount=1;VK_CHECK(vkAllocateCommandBuffers(device,&ca,&command));VkSemaphoreCreateInfo sem{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};VK_CHECK(vkCreateSemaphore(device,&sem,nullptr,&imageAvailable));for(size_t i=0;i<swapImages.size();i++){VkSemaphore rs;VK_CHECK(vkCreateSemaphore(device,&sem,nullptr,&rs));renderFinishedSemaphores.push_back(rs);}VkFenceCreateInfo fc{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};fc.flags=VK_FENCE_CREATE_SIGNALED_BIT;VK_CHECK(vkCreateFence(device,&fc,nullptr,&fence));
    }
    void render(const uint8_t* yPlane,const uint8_t* uPlane,const uint8_t* vPlane,jlong ptsNs,float x,float y,float z,int rotation) {
        const uint8_t* planes[3]={yPlane,uPlane,vPlane};const size_t sizes[3]={(size_t)inW*inH,(size_t)(inW/2)*(inH/2),(size_t)(inW/2)*(inH/2)};const uint32_t widths[3]={(uint32_t)inW,(uint32_t)inW/2,(uint32_t)inW/2};const uint32_t heights[3]={(uint32_t)inH,(uint32_t)inH/2,(uint32_t)inH/2};VK_CHECK(vkWaitForFences(device,1,&fence,VK_TRUE,UINT64_MAX));for(int i=0;i<3;i++)std::memcpy(stagingMaps[i],planes[i],sizes[i]);uint32_t imageIndex=0;VkResult acq=vkAcquireNextImageKHR(device,swapchain,UINT64_MAX,imageAvailable,VK_NULL_HANDLE,&imageIndex);if(acq!=VK_SUCCESS&&acq!=VK_SUBOPTIMAL_KHR)throw std::runtime_error("Vulkan gagal mengambil swapchain image");VK_CHECK(vkResetFences(device,1,&fence));VK_CHECK(vkResetCommandBuffer(command,0));VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};bi.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;VK_CHECK(vkBeginCommandBuffer(command,&bi));
        for(int i=0;i<3;i++){VkImageMemoryBarrier toDst{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};toDst.oldLayout=inputLayouts[i];toDst.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;toDst.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;toDst.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;toDst.image=inputImages[i];toDst.subresourceRange.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT;toDst.subresourceRange.levelCount=1;toDst.subresourceRange.layerCount=1;toDst.srcAccessMask=inputLayouts[i]==VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL?VK_ACCESS_SHADER_READ_BIT:0;toDst.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;vkCmdPipelineBarrier(command,inputLayouts[i]==VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL?VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT:VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&toDst);VkBufferImageCopy copy{};copy.imageSubresource.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT;copy.imageSubresource.layerCount=1;copy.imageExtent={widths[i],heights[i],1};vkCmdCopyBufferToImage(command,stagingBuffers[i],inputImages[i],VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);VkImageMemoryBarrier toRead{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};toRead.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;toRead.newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;toRead.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;toRead.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;toRead.image=inputImages[i];toRead.subresourceRange=toDst.subresourceRange;toRead.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;toRead.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,0,0,nullptr,0,nullptr,1,&toRead);inputLayouts[i]=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;}
        VkClearValue clear{};VkRenderPassBeginInfo rbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};rbi.renderPass=renderPass;rbi.framebuffer=framebuffers[imageIndex];rbi.renderArea.extent=extent;rbi.clearValueCount=1;rbi.pClearValues=&clear;vkCmdBeginRenderPass(command,&rbi,VK_SUBPASS_CONTENTS_INLINE);VkViewport viewport{0,0,(float)extent.width,(float)extent.height,0,1};VkRect2D scissor{{0,0},extent};vkCmdSetViewport(command,0,1,&viewport);vkCmdSetScissor(command,0,1,&scissor);vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline);vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_GRAPHICS,pipelineLayout,0,1,&descriptorSet,0,nullptr);// Camera-pivot mode: pitch/yaw are now global projective camera rotations,
        // not flat image translations. The Java path already applies 0.78 gain and
        // temporal smoothing, so do not weaken the correction again here.
        // Extra crop at the beginning hides transient empty borders while the gyro path settles.
        // Ease from 1.20x at the first frame to the normal 1.12x stabilization crop in 0.7 s.
        float t = std::max(0.0f, (float)ptsNs / 1000000000.0f);
        float u = std::min(1.0f, t / 0.70f);
        float ease = u * u * (3.0f - 2.0f * u);
        float zoom = 1.20f - 0.08f * ease;
        PushParams params{-z,y,-x,1.0f/zoom,(float)rotation,1.0f/std::max(1,outW),1.0f/std::max(1,outH)};vkCmdPushConstants(command,pipelineLayout,VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof(params),&params);vkCmdDraw(command,3,1,0,0);vkCmdEndRenderPass(command);VK_CHECK(vkEndCommandBuffer(command));
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};si.waitSemaphoreCount=1;si.pWaitSemaphores=&imageAvailable;VkPipelineStageFlags waitStage=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;si.pWaitDstStageMask=&waitStage;si.commandBufferCount=1;si.pCommandBuffers=&command;VkSemaphore renderFinished=renderFinishedSemaphores[imageIndex];si.signalSemaphoreCount=1;si.pSignalSemaphores=&renderFinished;VK_CHECK(vkQueueSubmit(queue,1,&si,fence));
        // ANativeWindow_setBuffersTimestamp is not part of the public Android NDK API.
        // Vulkan presentation to this ANativeWindow does not expose per-frame PTS here.
        // Keep ptsNs in the JNI interface for compatibility; encoder timestamp behavior
        // must be verified at runtime on-device.
        (void)ptsNs;
        VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};pi.waitSemaphoreCount=1;pi.pWaitSemaphores=&renderFinished;pi.swapchainCount=1;pi.pSwapchains=&swapchain;pi.pImageIndices=&imageIndex;VkResult pres=vkQueuePresentKHR(queue,&pi);if(pres!=VK_SUCCESS&&pres!=VK_SUBOPTIMAL_KHR)throw std::runtime_error("Vulkan gagal mengirim frame ke encoder");
    }
    void destroy() {
        if(device)vkDeviceWaitIdle(device);
        if(device){for(int i=0;i<3;i++)if(stagingMaps[i]&&stagingMemories[i])vkUnmapMemory(device,stagingMemories[i]);if(fence)vkDestroyFence(device,fence,nullptr);if(imageAvailable)vkDestroySemaphore(device,imageAvailable,nullptr);for(auto sem:renderFinishedSemaphores)vkDestroySemaphore(device,sem,nullptr);if(commandPool)vkDestroyCommandPool(device,commandPool,nullptr);for(auto f:framebuffers)vkDestroyFramebuffer(device,f,nullptr);if(pipeline)vkDestroyPipeline(device,pipeline,nullptr);if(pipelineLayout)vkDestroyPipelineLayout(device,pipelineLayout,nullptr);if(descriptorPool)vkDestroyDescriptorPool(device,descriptorPool,nullptr);if(descriptorLayout)vkDestroyDescriptorSetLayout(device,descriptorLayout,nullptr);if(sampler)vkDestroySampler(device,sampler,nullptr);for(int i=0;i<3;i++){if(inputViews[i])vkDestroyImageView(device,inputViews[i],nullptr);if(inputImages[i])vkDestroyImage(device,inputImages[i],nullptr);if(inputMemories[i])vkFreeMemory(device,inputMemories[i],nullptr);if(stagingBuffers[i])vkDestroyBuffer(device,stagingBuffers[i],nullptr);if(stagingMemories[i])vkFreeMemory(device,stagingMemories[i],nullptr);}for(auto v:swapViews)vkDestroyImageView(device,v,nullptr);if(renderPass)vkDestroyRenderPass(device,renderPass,nullptr);if(swapchain)vkDestroySwapchainKHR(device,swapchain,nullptr);vkDestroyDevice(device,nullptr);}
        if(surface&&instance)vkDestroySurfaceKHR(instance,surface,nullptr);if(instance)vkDestroyInstance(instance,nullptr);if(window)ANativeWindow_release(window);
    }
};
static void throwJava(JNIEnv* env,const std::string& msg){jclass c=env->FindClass("java/lang/RuntimeException");if(c)env->ThrowNew(c,msg.c_str());}
extern "C" JNIEXPORT jlong JNICALL Java_com_ridiitols_gyrolock_GyroVulkanRenderer_nativeCreate(JNIEnv* env,jclass,jobject surface,jint inW,jint inH,jint outW,jint outH,jobject assetManager){
    VulkanRenderer* r=nullptr;try{if(inW<=0||inH<=0||outW<=0||outH<=0)throw std::runtime_error("Resolusi Vulkan tidak valid");r=new VulkanRenderer();r->inW=inW;r->inH=inH;r->outW=outW;r->outH=outH;r->init(AAssetManager_fromJava(env,assetManager),surface,env);return (jlong)r;}catch(const std::exception& e){if(r){r->destroy();delete r;}throwJava(env,e.what());return 0;}}
extern "C" JNIEXPORT void JNICALL Java_com_ridiitols_gyrolock_GyroVulkanRenderer_nativeRender(JNIEnv* env,jclass,jlong handle,jobject yBuffer,jobject uBuffer,jobject vBuffer,jlong ptsNs,jfloat x,jfloat y,jfloat z,jint rotation){
    auto* r=(VulkanRenderer*)handle;try{if(!r)throw std::runtime_error("Renderer Vulkan belum dibuat");jobject bufs[3]={yBuffer,uBuffer,vBuffer};const size_t sizes[3]={(size_t)r->inW*r->inH,(size_t)(r->inW/2)*(r->inH/2),(size_t)(r->inW/2)*(r->inH/2)};const uint8_t* data[3]{};for(int i=0;i<3;i++){data[i]=(const uint8_t*)env->GetDirectBufferAddress(bufs[i]);jlong cap=env->GetDirectBufferCapacity(bufs[i]);if(!data[i]||cap<(jlong)sizes[i])throw std::runtime_error("Plane YUV Vulkan tidak valid");}r->render(data[0],data[1],data[2],ptsNs,x,y,z,rotation);}catch(const std::exception& e){throwJava(env,e.what());}}
extern "C" JNIEXPORT void JNICALL Java_com_ridiitols_gyrolock_GyroVulkanRenderer_nativeDestroy(JNIEnv*,jclass,jlong handle){auto* r=(VulkanRenderer*)handle;if(r){r->destroy();delete r;}}
