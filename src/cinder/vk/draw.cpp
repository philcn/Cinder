/*
 Copyright 2016 Google Inc.
 
 Licensed under the Apache License, Version 2.0 (the "License");
 you may not use this file except in compliance with the License.
 You may obtain a copy of the License at
 
 http://www.apache.org/licenses/LICENSE-2.0
 
 Unless required by applicable law or agreed to in writing, software
 distributed under the License is distributed on an "AS IS" BASIS,
 WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 See the License for the specific language governing permissions and
 limitations under the License.


 Copyright (c) 2016, The Cinder Project, All rights reserved.

 This code is intended for use with the Cinder C++ library: http://libcinder.org

 Redistribution and use in source and binary forms, with or without modification, are permitted provided that
 the following conditions are met:

    * Redistributions of source code must retain the above copyright notice, this list of conditions and
	the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright notice, this list of conditions and
	the following disclaimer in the documentation and/or other materials provided with the distribution.

 THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED
 WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
 PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
 ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED
 TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 POSSIBILITY OF SUCH DAMAGE.
*/

#include "cinder/vk/draw.h"
#include "cinder/vk/CommandPool.h"
#include "cinder/vk/CommandBuffer.h"
#include "cinder/vk/Context.h"
#include "cinder/vk/Descriptor.h"
#include "cinder/vk/Device.h"
#include "cinder/vk/PipelineSelector.h"
#include "cinder/vk/RenderPass.h"
#include "cinder/vk/ShaderProg.h"
#include "cinder/vk/Texture.h"
#include "cinder/vk/UniformBuffer.h"
#include "cinder/vk/UniformLayout.h"
#include "cinder/vk/VertexBuffer.h"
#include "cinder/vk/wrapper.h"

namespace cinder { namespace vk {

void draw( const Texture2dRef &texture, const Rectf &dstRect, const std::string& uniformName )
{
	struct DrawCache {
		vk::ShaderProgRef			shader;
		vk::Texture2dRef			texture;
		VkPipelineLayout			pipelineLayout = VK_NULL_HANDLE;
		VkPipeline					pipeline = VK_NULL_HANDLE;
		bool						updateSetBindings = false;
		DrawCache() {}
	};

	static vk::VertexBufferRef sVertexBufferCache;
	static std::shared_ptr<DrawCache> sDrawCache;

	vk::UniformSetRef transientUniformSet;
	vk::DescriptorSetViewRef transientDescriptorView;

	// Handle caching
	{
		// Cache vertex buffer
		if( ! sVertexBufferCache ) {
			// Triangle strip
			std::vector<float> data = {
				0.0f, 0.0f, 0.0f, 1.0f,
				0.0f, 1.0f, 0.0f, 1.0f,
				1.0f, 0.0f, 0.0f, 1.0f,
				1.0f, 1.0f, 0.0f, 1.0f,
			};
			// Vertex buffer
			sVertexBufferCache = vk::VertexBuffer::create( static_cast<const void*>( data.data() ), data.size()*sizeof( float ), vk::VertexBuffer::Format().setTransientAllocation() );
		}

		if( ! sDrawCache ) {
			sDrawCache = std::shared_ptr<DrawCache>( new DrawCache() );
		}

		const auto& shader = vk::context()->getShaderProg();
		if( ( shader != sDrawCache->shader ) || ( texture != sDrawCache->texture ) ) {
			// Update cache
			sDrawCache->shader = shader;
			sDrawCache->texture = texture;
			// Create descriptor view
			const vk::UniformLayout& uniformLayout = sDrawCache->shader->getUniformLayout();			
			transientUniformSet = vk::UniformSet::create( uniformLayout );
			transientUniformSet->uniform( uniformName, sDrawCache->texture );
			vk::context()->addTransient( transientUniformSet );
			std::vector<VkDescriptorSetLayout> descriptorSetLayouts = vk::context()->getDevice()->getDescriptorSetLayoutSelector()->getSelectedLayout( transientUniformSet->getCachedDescriptorSetLayoutBindings() );
			transientDescriptorView = vk::DescriptorSetView::create( transientUniformSet );
			transientDescriptorView->allocateDescriptorSets();
			transientDescriptorView->updateDescriptorSets();
			vk::context()->addTransient( transientDescriptorView );
			// Update pipeline layout

			//const auto& descriptorSetLayouts = transientDescriptorView->getCachedDescriptorSetLayouts();
			const auto& pushConstantRanges = sDrawCache->shader->getCachedPushConstantRanges();
			sDrawCache->pipelineLayout = vk::context()->getDevice()->getPipelineLayoutSelector()->getSelectedLayout( descriptorSetLayouts, pushConstantRanges );
			// Set update flag
			sDrawCache->updateSetBindings = true;
		}

		// Pipeline
		{
			// Vertex input attribute description
			size_t stride = 0;
			std::vector<VkVertexInputAttributeDescription> viads;
			for( const auto& attrib : shader->getActiveAttributes() ) {
				VkVertexInputAttributeDescription viad = {};
				viad.location = attrib.getLocation();
				viad.binding  = attrib.getBinding();
				viad.format   = toVkFormat( attrib.getType() );
				viad.offset   = static_cast<uint32_t>( stride );
				viads.push_back( viad );
				size_t sizeBytes = vk::formatSizeBytes( viad.format );
				stride += sizeBytes;
			}

			// Vertex input binding description
			VkVertexInputBindingDescription vibd = {};
			vibd.binding	= 0;
			vibd.inputRate	= VK_VERTEX_INPUT_RATE_VERTEX;
			vibd.stride		= static_cast<uint32_t>( stride );

			auto ctx = vk::context();
			auto& pipelineSelector = ctx->getDevice()->getPipelineSelector();
			pipelineSelector->setTopology( VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP );
			pipelineSelector->setVertexInputAttributeDescriptions( viads );
			pipelineSelector->setVertexInputBindingDescriptions( { vibd }  );
			pipelineSelector->setCullMode( ctx->getCullMode() );
			pipelineSelector->setFrontFace( ctx->getFrontFace() );
			pipelineSelector->setDepthBias( ctx->getDepthBiasEnable(), ctx->getDepthBiasSlopeFactor(), ctx->getDepthBiasConstantFactor(), ctx->getDepthBiasClamp() );
			pipelineSelector->setRasterizationSamples( ctx->getRenderPass()->getSubpassSampleCount( ctx->getSubpass() ) );
			pipelineSelector->setDepthTest( ctx->getDepthTest() );
			pipelineSelector->setDepthWrite( ctx->getDepthWrite() );
			pipelineSelector->setColorBlendAttachments( ctx->getColorBlendAttachments() );
			pipelineSelector->setShaderStages( sDrawCache->shader->getPipelineShaderStages() );
			pipelineSelector->setRenderPass( ctx->getRenderPass()->getRenderPass() );
			pipelineSelector->setSubPass( ctx->getSubpass() );
			pipelineSelector->setPipelineLayout( sDrawCache->pipelineLayout );
			sDrawCache->pipeline = pipelineSelector->getSelectedPipeline();
		}
	}

	// Get current command buffer
	auto cmdBufRef = vk::context()->getCommandBuffer();
	auto cmdBuf = cmdBufRef->getCommandBuffer();

	// Handle descriptors
	if( sDrawCache->updateSetBindings ) {
		sDrawCache->updateSetBindings = false;
		const auto& descriptorSets = transientDescriptorView->getDescriptorSets();
		for( uint32_t i = 0; i < descriptorSets.size(); ++i ) {
			const auto& ds = descriptorSets[i];
			std::vector<VkDescriptorSet> descSets = { ds->vkObject() };
			vkCmdBindDescriptorSets( cmdBuf, VK_PIPELINE_BIND_POINT_GRAPHICS, sDrawCache->pipelineLayout, i, static_cast<uint32_t>( descSets.size() ), descSets.data(), 0, nullptr );
		}
	}

	// Push model view projection matrix
	VkPushConstantRange pcr = sDrawCache->shader->getCachedPushConstantRange( "ciBlock0.ciModelViewProjection" );
	if( 0 != pcr.stageFlags ) {
		mat4 mvp = vk::getModelViewProjection();
		vkCmdPushConstants( cmdBuf, sDrawCache->pipelineLayout, pcr.stageFlags, pcr.offset, pcr.size, &mvp );
	}
	// Push rect
	pcr = sDrawCache->shader->getCachedPushConstantRange( "ciBlock0.ciRect" );
	if( 0 != pcr.stageFlags ) {
		vkCmdPushConstants( cmdBuf, sDrawCache->pipelineLayout, pcr.stageFlags, pcr.offset, pcr.size, &dstRect );
	}
	// Push texture coords
	pcr = sDrawCache->shader->getCachedPushConstantRange( "ciBlock0.ciTexCoord" );
	if( 0 != pcr.stageFlags ) {
		vec2 texCoords[2] = { vec2( 0, 0 ), vec2( 1, 1 ) };
		vkCmdPushConstants( cmdBuf, sDrawCache->pipelineLayout, pcr.stageFlags, pcr.offset, pcr.size, texCoords );
	}
	// Push color
	pcr = sDrawCache->shader->getCachedPushConstantRange( "ciBlock0.ciColor" );
	if( 0 != pcr.stageFlags ) {
		const ColorAf& color = vk::context()->getCurrentColor();
		vkCmdPushConstants( cmdBuf, sDrawCache->pipelineLayout, pcr.stageFlags, pcr.offset, pcr.size, &color );
	}

	// Bind vertex buffer
	std::vector<VkBuffer> vertexBuffers = { sVertexBufferCache->getBuffer() };
	std::vector<VkDeviceSize> offsets = { 0 };
	vkCmdBindVertexBuffers( cmdBuf, 0, static_cast<uint32_t>( vertexBuffers.size() ), vertexBuffers.data(), offsets.data() );

	// Bind pipeline
	vkCmdBindPipeline( cmdBuf, VK_PIPELINE_BIND_POINT_GRAPHICS, sDrawCache->pipeline );

	// Draw geometry
	uint32_t numVertices = 4;
	vkCmdDraw( cmdBuf, numVertices, 1, 0, 0 );
}

void drawSolidRect( const Rectf &r, const vec2 &upperLeftTexCoord, const vec2 &lowerRightTexCoord )
{
	struct DrawCache {
		vk::ShaderProgRef	shader;
		VkPipelineLayout	pipelineLayout = VK_NULL_HANDLE;
		VkPipeline			pipeline = VK_NULL_HANDLE;
		DrawCache() {}
	};

	static vk::VertexBufferRef sVertexBufferCache;
	static std::shared_ptr<DrawCache> sDrawCache;

	// Handle caching
	{
		// Cache vertex buffer
		if( ! sVertexBufferCache ) {
			// Triangle strip
			std::vector<float> data = {
				0.0f, 0.0f, 0.0f, 1.0f,
				0.0f, 1.0f, 0.0f, 1.0f,
				1.0f, 0.0f, 0.0f, 1.0f,
				1.0f, 1.0f, 0.0f, 1.0f,
			};
			// Vertex buffer
			sVertexBufferCache = vk::VertexBuffer::create( static_cast<const void*>( data.data() ), data.size()*sizeof( float ), vk::VertexBuffer::Format().setTransientAllocation() );
		}

		if( ! sDrawCache ) {
			sDrawCache = std::shared_ptr<DrawCache>( new DrawCache() );
		}

		const auto& shader = vk::context()->getShaderProg();
		if( shader != sDrawCache->shader ) {
			// Cache shader
			sDrawCache->shader = shader;
			// Cache pipeline layout
			const auto& pushConstantRanges = shader->getCachedPushConstantRanges();
			sDrawCache->pipelineLayout = vk::context()->getDevice()->getPipelineLayoutSelector()->getSelectedLayout( pushConstantRanges );
		}

		// Pipeline
		{
			// Vertex input attribute description
			size_t stride = 0;
			std::vector<VkVertexInputAttributeDescription> viads;
			for( const auto& attrib : shader->getActiveAttributes() ) {
				VkVertexInputAttributeDescription viad = {};
				viad.location = attrib.getLocation();
				viad.binding  = attrib.getBinding();
				viad.format   = toVkFormat( attrib.getType() );
				viad.offset   = static_cast<uint32_t>( stride );
				viads.push_back( viad );
				size_t sizeBytes = vk::formatSizeBytes( viad.format );
				stride += sizeBytes;
			}

			// Vertex input binding description
			VkVertexInputBindingDescription vibd = {};
			vibd.binding	= 0;
			vibd.inputRate	= VK_VERTEX_INPUT_RATE_VERTEX;
			vibd.stride		= static_cast<uint32_t>( stride );

			auto ctx = vk::context();
			auto& pipelineSelector = ctx->getDevice()->getPipelineSelector();
			pipelineSelector->setTopology( VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP );
			pipelineSelector->setVertexInputAttributeDescriptions( viads );
			pipelineSelector->setVertexInputBindingDescriptions( { vibd }  );
			pipelineSelector->setCullMode( ctx->getCullMode() );
			pipelineSelector->setFrontFace( ctx->getFrontFace() );
			pipelineSelector->setDepthBias( ctx->getDepthBiasEnable(), ctx->getDepthBiasSlopeFactor(), ctx->getDepthBiasConstantFactor(), ctx->getDepthBiasClamp() );
			pipelineSelector->setRasterizationSamples( ctx->getRenderPass()->getSubpassSampleCount( ctx->getSubpass() ) );
			pipelineSelector->setDepthTest( ctx->getDepthTest() );
			pipelineSelector->setDepthWrite( ctx->getDepthWrite() );
			pipelineSelector->setColorBlendAttachments( ctx->getColorBlendAttachments() );
			pipelineSelector->setShaderStages( sDrawCache->shader->getPipelineShaderStages() );
			pipelineSelector->setRenderPass( ctx->getRenderPass()->getRenderPass() );
			pipelineSelector->setSubPass( ctx->getSubpass() );
			pipelineSelector->setPipelineLayout( sDrawCache->pipelineLayout );
			sDrawCache->pipeline = pipelineSelector->getSelectedPipeline();
		}
	}

	// Get current command buffer
	auto cmdBufRef = vk::context()->getCommandBuffer();
	auto cmdBuf = cmdBufRef->getCommandBuffer();

	// Push model view projection matrix
	VkPushConstantRange pcr = sDrawCache->shader->getCachedPushConstantRange( "ciBlock0.ciModelViewProjection" );
	if( 0 != pcr.stageFlags ) {
		mat4 mvp = vk::getModelViewProjection();
		vkCmdPushConstants( cmdBuf, sDrawCache->pipelineLayout, pcr.stageFlags, pcr.offset, pcr.size, &mvp );
	}
	// Push rect
	pcr = sDrawCache->shader->getCachedPushConstantRange( "ciBlock0.ciRect" );
	if( 0 != pcr.stageFlags ) {
		vkCmdPushConstants( cmdBuf, sDrawCache->pipelineLayout, pcr.stageFlags, pcr.offset, pcr.size, &r );
	}
	// Push texture coords
	pcr = sDrawCache->shader->getCachedPushConstantRange( "ciBlock0.ciTexCoord" );
	if( 0 != pcr.stageFlags ) {
		vec2 texCoords[2] = { upperLeftTexCoord, lowerRightTexCoord };
		vkCmdPushConstants( cmdBuf, sDrawCache->pipelineLayout, pcr.stageFlags, pcr.offset, pcr.size, texCoords );
	}
	// Push color
	pcr = sDrawCache->shader->getCachedPushConstantRange( "ciBlock0.ciColor" );
	if( 0 != pcr.stageFlags ) {
		const ColorAf& color = vk::context()->getCurrentColor();
		vkCmdPushConstants( cmdBuf, sDrawCache->pipelineLayout, pcr.stageFlags, pcr.offset, pcr.size, &color );
	}

	// Bind vertex buffer
	std::vector<VkBuffer> vertexBuffers = { sVertexBufferCache->getBuffer() };
	std::vector<VkDeviceSize> offsets = { 0 };
	vkCmdBindVertexBuffers( cmdBuf, 0, static_cast<uint32_t>( vertexBuffers.size() ), vertexBuffers.data(), offsets.data() );

	// Bind pipeline
	vkCmdBindPipeline( cmdBuf, VK_PIPELINE_BIND_POINT_GRAPHICS, sDrawCache->pipeline );

	// Draw geometry
	uint32_t numVertices = 4;
	vkCmdDraw( cmdBuf, numVertices, 1, 0, 0 );
}

}} // namespace cinder::vk