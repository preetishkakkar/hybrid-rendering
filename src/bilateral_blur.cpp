#include "bilateral_blur.h"
#include "g_buffer.h"
#include "common_resources.h"
#include "utilities.h"
#include <macros.h>
#include <profiler.h>
#include <imgui.h>

BilateralBlur::BilateralBlur(std::weak_ptr<dw::vk::Backend> backend, CommonResources* common_resources, GBuffer* g_buffer, std::string name, uint32_t input_width, uint32_t input_height) :
    m_name(name), m_backend(backend), m_common_resources(common_resources), m_g_buffer(g_buffer), m_input_width(input_width), m_input_height(input_height)
{
    auto vk_backend = backend.lock();
    auto extents    = vk_backend->swap_chain_extents();

    m_scale = float(extents.width) / float(m_input_width);

    m_image = dw::vk::Image::create(vk_backend, VK_IMAGE_TYPE_2D, input_width, input_height, 1, 1, 1, VK_FORMAT_R16G16B16A16_SFLOAT, VMA_MEMORY_USAGE_GPU_ONLY, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT, VK_SAMPLE_COUNT_1_BIT);
    m_image->set_name(name + " Bilateral");

    m_image_view = dw::vk::ImageView::create(vk_backend, m_image, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_COLOR_BIT);
    m_image_view->set_name(name + " Bilateral");

    m_write_ds = vk_backend->allocate_descriptor_set(m_common_resources->storage_image_ds_layout);
    m_read_ds  = vk_backend->allocate_descriptor_set(m_common_resources->combined_sampler_ds_layout);

    // write
    {
        VkDescriptorImageInfo storage_image_info;

        storage_image_info.sampler     = VK_NULL_HANDLE;
        storage_image_info.imageView   = m_image_view->handle();
        storage_image_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkWriteDescriptorSet write_data;

        DW_ZERO_MEMORY(write_data);

        write_data.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write_data.descriptorCount = 1;
        write_data.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        write_data.pImageInfo      = &storage_image_info;
        write_data.dstBinding      = 0;
        write_data.dstSet          = m_write_ds->handle();

        vkUpdateDescriptorSets(vk_backend->device(), 1, &write_data, 0, nullptr);
    }

    // read
    {
        VkDescriptorImageInfo sampler_image_info;

        sampler_image_info.sampler     = vk_backend->bilinear_sampler()->handle();
        sampler_image_info.imageView   = m_image_view->handle();
        sampler_image_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkWriteDescriptorSet write_data;

        DW_ZERO_MEMORY(write_data);

        write_data.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write_data.descriptorCount = 1;
        write_data.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write_data.pImageInfo      = &sampler_image_info;
        write_data.dstBinding      = 0;
        write_data.dstSet          = m_read_ds->handle();

        vkUpdateDescriptorSets(vk_backend->device(), 1, &write_data, 0, nullptr);
    }

    dw::vk::PipelineLayout::Desc desc;

    desc.add_descriptor_set_layout(m_common_resources->storage_image_ds_layout);
    desc.add_descriptor_set_layout(m_common_resources->combined_sampler_ds_layout);
    desc.add_descriptor_set_layout(m_g_buffer->ds_layout());

    desc.add_push_constant_range(VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(PushConstants));

    m_layout = dw::vk::PipelineLayout::create(vk_backend, desc);

    dw::vk::ShaderModule::Ptr module = dw::vk::ShaderModule::create_from_file(vk_backend, "shaders/bilateral_blur.comp.spv");

    dw::vk::ComputePipeline::Desc comp_desc;

    comp_desc.set_pipeline_layout(m_layout);
    comp_desc.set_shader_stage(module, "main");

    m_pipeline = dw::vk::ComputePipeline::create(vk_backend, comp_desc);
}

BilateralBlur::~BilateralBlur()
{
}

void BilateralBlur::blur(dw::vk::CommandBuffer::Ptr cmd_buf, dw::vk::DescriptorSet::Ptr input)
{
    DW_SCOPED_SAMPLE(m_name + " Bilateral Blur", cmd_buf);

    VkImageSubresourceRange subresource_range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

    dw::vk::utilities::set_image_layout(
        cmd_buf->handle(),
        m_image->handle(),
        VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_GENERAL,
        subresource_range);

    const uint32_t NUM_THREADS = 32;

    vkCmdBindPipeline(cmd_buf->handle(), VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline->handle());

    PushConstants push_constants;

    push_constants.z_buffer_params     = m_common_resources->z_buffer_params;
    push_constants.variance_threshold  = m_variance_threshold;
    push_constants.roughness_sigma_min = m_roughness_sigma_min;
    push_constants.roughness_sigma_max = m_roughness_sigma_max;
    push_constants.radius              = m_blur_radius;
    push_constants.roughness_weight    = (uint32_t)m_use_roughness_weight;
    push_constants.depth_weight        = (uint32_t)m_use_depth_weight;
    push_constants.normal_weight       = (uint32_t)m_use_normal_weight;
    push_constants.g_buffer_mip        = m_scale == 1.0f ? 0 : 1;

    vkCmdPushConstants(cmd_buf->handle(), m_layout->handle(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push_constants), &push_constants);

    VkDescriptorSet descriptor_sets[] = {
        m_write_ds->handle(),
        input->handle(),
        m_g_buffer->output_ds()->handle()
    };

    vkCmdBindDescriptorSets(cmd_buf->handle(), VK_PIPELINE_BIND_POINT_COMPUTE, m_layout->handle(), 0, 3, descriptor_sets, 0, nullptr);

    vkCmdDispatch(cmd_buf->handle(), static_cast<uint32_t>(ceil(float(m_image->width()) / float(NUM_THREADS))), static_cast<uint32_t>(ceil(float(m_image->height()) / float(NUM_THREADS))), 1);

    dw::vk::utilities::set_image_layout(
        cmd_buf->handle(),
        m_image->handle(),
        VK_IMAGE_LAYOUT_GENERAL,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        subresource_range);
}

void BilateralBlur::prepare_first_frame(dw::vk::CommandBuffer::Ptr cmd_buf)
{
    VkImageSubresourceRange subresource_range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

    dw::vk::utilities::set_image_layout(
        cmd_buf->handle(),
        m_image->handle(),
        VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        subresource_range);
}

void BilateralBlur::gui()
{
    ImGui::Checkbox("Depth Weight", &m_use_depth_weight);
    ImGui::Checkbox("Normal Weight", &m_use_normal_weight);
    ImGui::Checkbox("Roughness Weight", &m_use_roughness_weight);
    ImGui::SliderInt("Radius", &m_blur_radius, 1, 10);
    ImGui::SliderFloat("Variance Threshold", &m_variance_threshold, 0.0f, 1.0f);
    ImGui::InputFloat("Roughness Sigma Min", &m_roughness_sigma_min);
    ImGui::InputFloat("Roughness Sigma Max", &m_roughness_sigma_max);
}

dw::vk::DescriptorSet::Ptr BilateralBlur::output_ds()
{
    return m_read_ds;
}