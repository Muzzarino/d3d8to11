/**
 * Copyright (C) 2015 Patrick Mours. All rights reserved.
 * License: https://github.com/crosire/d3d8to9#license
 */

#include "stdafx.h"

#include <d3d11_1.h> // TODO: switch to newer header (11.3, 11.4)
#include <DirectXMath.h>
#include <cassert>
#include <fstream>
#include <optional>
#include <sstream>
#include <filesystem>

#include "d3d8to9.hpp"
#include "SimpleMath.h"
#include "int_multiple.h"
#include "CBufferWriter.h"
#include "Material.h"
#include "ShaderIncluder.h"
#include "safe_release.h"
#include "globals.h"
#include "ini_file.h"

// TODO: provide a wrapper structure that can swap out render targets when OIT is toggled

#define SHADER_ASYNC_COMPILE
//#define SHADER_FAST_FALLBACK

#define LOCK(MUTEX) std::lock_guard<decltype(MUTEX)> MUTEX ## _guard(MUTEX)

using namespace Microsoft::WRL;
using namespace d3d8to11;

static constexpr uint32_t BLEND_COLORMASK_SHIFT = 28;

static std::unordered_map<uint32_t, std::string> rs_strings = {
	{ D3DRS_ZENABLE,                  "D3DRS_ZENABLE" },
	{ D3DRS_FILLMODE,                 "D3DRS_FILLMODE" },
	{ D3DRS_SHADEMODE,                "D3DRS_SHADEMODE" },
	{ D3DRS_LINEPATTERN,              "D3DRS_LINEPATTERN" },
	{ D3DRS_ZWRITEENABLE,             "D3DRS_ZWRITEENABLE" },
	{ D3DRS_ALPHATESTENABLE,          "D3DRS_ALPHATESTENABLE" },
	{ D3DRS_LASTPIXEL,                "D3DRS_LASTPIXEL" },
	{ D3DRS_SRCBLEND,                 "D3DRS_SRCBLEND" },
	{ D3DRS_DESTBLEND,                "D3DRS_DESTBLEND" },
	{ D3DRS_CULLMODE,                 "D3DRS_CULLMODE" },
	{ D3DRS_ZFUNC,                    "D3DRS_ZFUNC" },
	{ D3DRS_ALPHAREF,                 "D3DRS_ALPHAREF" },
	{ D3DRS_ALPHAFUNC,                "D3DRS_ALPHAFUNC" },
	{ D3DRS_DITHERENABLE,             "D3DRS_DITHERENABLE" },
	{ D3DRS_ALPHABLENDENABLE,         "D3DRS_ALPHABLENDENABLE" },
	{ D3DRS_FOGENABLE,                "D3DRS_FOGENABLE" },
	{ D3DRS_SPECULARENABLE,           "D3DRS_SPECULARENABLE" },
	{ D3DRS_ZVISIBLE,                 "D3DRS_ZVISIBLE" },
	{ D3DRS_FOGCOLOR,                 "D3DRS_FOGCOLOR" },
	{ D3DRS_FOGTABLEMODE,             "D3DRS_FOGTABLEMODE" },
	{ D3DRS_FOGSTART,                 "D3DRS_FOGSTART" },
	{ D3DRS_FOGEND,                   "D3DRS_FOGEND" },
	{ D3DRS_FOGDENSITY,               "D3DRS_FOGDENSITY" },
	{ D3DRS_EDGEANTIALIAS,            "D3DRS_EDGEANTIALIAS" },
	{ D3DRS_ZBIAS,                    "D3DRS_ZBIAS" },
	{ D3DRS_RANGEFOGENABLE,           "D3DRS_RANGEFOGENABLE" },
	{ D3DRS_STENCILENABLE,            "D3DRS_STENCILENABLE" },
	{ D3DRS_STENCILFAIL,              "D3DRS_STENCILFAIL" },
	{ D3DRS_STENCILZFAIL,             "D3DRS_STENCILZFAIL" },
	{ D3DRS_STENCILPASS,              "D3DRS_STENCILPASS" },
	{ D3DRS_STENCILFUNC,              "D3DRS_STENCILFUNC" },
	{ D3DRS_STENCILREF,               "D3DRS_STENCILREF" },
	{ D3DRS_STENCILMASK,              "D3DRS_STENCILMASK" },
	{ D3DRS_STENCILWRITEMASK,         "D3DRS_STENCILWRITEMASK" },
	{ D3DRS_TEXTUREFACTOR,            "D3DRS_TEXTUREFACTOR" },
	{ D3DRS_WRAP0,                    "D3DRS_WRAP0" },
	{ D3DRS_WRAP1,                    "D3DRS_WRAP1" },
	{ D3DRS_WRAP2,                    "D3DRS_WRAP2" },
	{ D3DRS_WRAP3,                    "D3DRS_WRAP3" },
	{ D3DRS_WRAP4,                    "D3DRS_WRAP4" },
	{ D3DRS_WRAP5,                    "D3DRS_WRAP5" },
	{ D3DRS_WRAP6,                    "D3DRS_WRAP6" },
	{ D3DRS_WRAP7,                    "D3DRS_WRAP7" },
	{ D3DRS_CLIPPING,                 "D3DRS_CLIPPING" },
	{ D3DRS_LIGHTING,                 "D3DRS_LIGHTING" },
	{ D3DRS_AMBIENT,                  "D3DRS_AMBIENT" },
	{ D3DRS_FOGVERTEXMODE,            "D3DRS_FOGVERTEXMODE" },
	{ D3DRS_COLORVERTEX,              "D3DRS_COLORVERTEX" },
	{ D3DRS_LOCALVIEWER,              "D3DRS_LOCALVIEWER" },
	{ D3DRS_NORMALIZENORMALS,         "D3DRS_NORMALIZENORMALS" },
	{ D3DRS_DIFFUSEMATERIALSOURCE,    "D3DRS_DIFFUSEMATERIALSOURCE" },
	{ D3DRS_SPECULARMATERIALSOURCE,   "D3DRS_SPECULARMATERIALSOURCE" },
	{ D3DRS_AMBIENTMATERIALSOURCE,    "D3DRS_AMBIENTMATERIALSOURCE" },
	{ D3DRS_EMISSIVEMATERIALSOURCE,   "D3DRS_EMISSIVEMATERIALSOURCE" },
	{ D3DRS_VERTEXBLEND,              "D3DRS_VERTEXBLEND" },
	{ D3DRS_CLIPPLANEENABLE,          "D3DRS_CLIPPLANEENABLE" },
	{ D3DRS_SOFTWAREVERTEXPROCESSING, "D3DRS_SOFTWAREVERTEXPROCESSING" },
	{ D3DRS_POINTSIZE,                "D3DRS_POINTSIZE" },
	{ D3DRS_POINTSIZE_MIN,            "D3DRS_POINTSIZE_MIN" },
	{ D3DRS_POINTSPRITEENABLE,        "D3DRS_POINTSPRITEENABLE" },
	{ D3DRS_POINTSCALEENABLE,         "D3DRS_POINTSCALEENABLE" },
	{ D3DRS_POINTSCALE_A,             "D3DRS_POINTSCALE_A" },
	{ D3DRS_POINTSCALE_B,             "D3DRS_POINTSCALE_B" },
	{ D3DRS_POINTSCALE_C,             "D3DRS_POINTSCALE_C" },
	{ D3DRS_MULTISAMPLEANTIALIAS,     "D3DRS_MULTISAMPLEANTIALIAS" },
	{ D3DRS_MULTISAMPLEMASK,          "D3DRS_MULTISAMPLEMASK" },
	{ D3DRS_PATCHEDGESTYLE,           "D3DRS_PATCHEDGESTYLE" },
	{ D3DRS_PATCHSEGMENTS,            "D3DRS_PATCHSEGMENTS" },
	{ D3DRS_DEBUGMONITORTOKEN,        "D3DRS_DEBUGMONITORTOKEN" },
	{ D3DRS_POINTSIZE_MAX,            "D3DRS_POINTSIZE_MAX" },
	{ D3DRS_INDEXEDVERTEXBLENDENABLE, "D3DRS_INDEXEDVERTEXBLENDENABLE" },
	{ D3DRS_COLORWRITEENABLE,         "D3DRS_COLORWRITEENABLE" },
	{ D3DRS_TWEENFACTOR,              "D3DRS_TWEENFACTOR" },
	{ D3DRS_BLENDOP,                  "D3DRS_BLENDOP" },
	{ D3DRS_POSITIONORDER,            "D3DRS_POSITIONORDER" },
	{ D3DRS_NORMALORDER,              "D3DRS_NORMALORDER" }
};

static const std::array<D3D_FEATURE_LEVEL, 4> FEATURE_LEVELS =
{
	D3D_FEATURE_LEVEL_12_1,
	D3D_FEATURE_LEVEL_12_0,
	D3D_FEATURE_LEVEL_11_1,
	D3D_FEATURE_LEVEL_11_0
};

inline uint32_t fvf_sanitize(uint32_t value)
{
	if ((value & D3DFVF_XYZ) == D3DFVF_XYZ)
	{
		value &= ~D3DFVF_XYZRHW;
	}
	else if ((value & D3DFVF_XYZRHW) == D3DFVF_XYZRHW)
	{
		value &= ~D3DFVF_NORMAL;
	}

	return value;
}

size_t Direct3DDevice8::count_texture_stages() const
{
	size_t n = 0;

	for (const auto& stage : per_texture.stages)
	{
		if (stage.color_op.data() == D3DTOP_DISABLE && stage.alpha_op.data() == D3DTOP_DISABLE)
		{
			break;
		}

		++n;
	}

	return n;
}

#define STRINGIFY(x) #x
#define TOSTRING(x) STRINGIFY(x)

static std::unordered_map<size_t, std::string> digit_strings;

const std::vector<D3D_SHADER_MACRO>& Direct3DDevice8::shader_preprocess(ShaderFlags::type flags_)
{
	static std::array<const char*, 8> texcoord_size_strings = {
		"FVF_TEXCOORD0_SIZE",
		"FVF_TEXCOORD1_SIZE",
		"FVF_TEXCOORD2_SIZE",
		"FVF_TEXCOORD3_SIZE",
		"FVF_TEXCOORD4_SIZE",
		"FVF_TEXCOORD5_SIZE",
		"FVF_TEXCOORD6_SIZE",
		"FVF_TEXCOORD7_SIZE"
	};

	static std::array<const char*, 8> texcoord_size_types = {
		"FVF_TEXCOORD0_TYPE",
		"FVF_TEXCOORD1_TYPE",
		"FVF_TEXCOORD2_TYPE",
		"FVF_TEXCOORD3_TYPE",
		"FVF_TEXCOORD4_TYPE",
		"FVF_TEXCOORD5_TYPE",
		"FVF_TEXCOORD6_TYPE",
		"FVF_TEXCOORD7_TYPE"
	};

	static std::array<const char*, 4> texcoord_format_types = {
		"float2",
		"float3",
		"float4",
		"float1",
	};

	auto flags = ShaderFlags::sanitize(flags_);
	flags &= ~ShaderFlags::stage_count;
	flags |= (static_cast<ShaderFlags::type>(count_texture_stages()) << ShaderFlags::stage_count_shift) & ShaderFlags::stage_count;

	std::lock_guard shader_preproc_lock(shader_preproc_mutex);

	auto it = shader_preproc_definitions.find(flags);

	if (it != shader_preproc_definitions.end())
	{
		return it->second;
	}

	std::vector<D3D_SHADER_MACRO> definitions
	{
		{ "MAX_FRAGMENTS", fragments_str.c_str() },
		{ "TEXTURE_STAGE_MAX", TOSTRING(TEXTURE_STAGE_MAX) }
	};

	auto format = flags >> 16u;
	auto tex_count = static_cast<size_t>(((flags & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT) & 0xF);

	for (size_t i = 0; i < tex_count; i++)
	{
		const auto f = static_cast<size_t>(format & 3u);

		switch (f)
		{
			case D3DFVF_TEXTUREFORMAT2:
				definitions.push_back({ texcoord_size_strings[i], "2" });
				break;

			case D3DFVF_TEXTUREFORMAT1:
				definitions.push_back({ texcoord_size_strings[i], "1" });
				break;

			case D3DFVF_TEXTUREFORMAT3:
				definitions.push_back({ texcoord_size_strings[i], "3" });
				break;

			case D3DFVF_TEXTUREFORMAT4:
				definitions.push_back({ texcoord_size_strings[i], "4" });
				break;

			default:
				break;
		}

		switch (f)
		{
			case D3DFVF_TEXTUREFORMAT2:
			case D3DFVF_TEXTUREFORMAT1:
			case D3DFVF_TEXTUREFORMAT3:
			case D3DFVF_TEXTUREFORMAT4:
				definitions.push_back({ texcoord_size_types[i], texcoord_format_types[f] });
				break;

			default:
				break;
		}

		format >>= 2;
	}

	const size_t stage_count = (flags >> ShaderFlags::stage_count_shift) & 0xF;
	auto digit_string = digit_strings.find(stage_count);

	if (digit_string == digit_strings.end())
	{
		digit_strings[stage_count] = std::to_string(stage_count);
		digit_string = digit_strings.find(stage_count);

		std::stringstream ss;
		ss << "generating shader with texture stage count: " << stage_count;

		OutputDebugStringA(ss.str().c_str());
	}

	definitions.push_back({ "TEXTURE_STAGE_COUNT", digit_string->second.c_str() });

	if ((flags & D3DFVF_POSITION_MASK) == D3DFVF_XYZRHW)
	{
		definitions.push_back({ "FVF_RHW", "1" });
	}

	if ((flags & D3DFVF_POSITION_MASK) == D3DFVF_XYZ)
	{
		definitions.push_back({ "FVF_XYZ", "1" });
	}

	if (flags & D3DFVF_NORMAL)
	{
		definitions.push_back({ "FVF_NORMAL", "1" });
	}

	if (flags & D3DFVF_DIFFUSE)
	{
		definitions.push_back({ "FVF_DIFFUSE", "1" });
	}

	if (flags & D3DFVF_SPECULAR)
	{
		definitions.push_back({ "FVF_SPECULAR", "1" });
	}

	if (flags & D3DFVF_TEXCOUNT_MASK)
	{
		texcount_str = std::to_string((flags & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT);
		definitions.push_back({ "FVF_TEXCOUNT", texcount_str.c_str() });
	}

	if ((flags & (ShaderFlags::rs_lighting | D3DFVF_NORMAL)) == (ShaderFlags::rs_lighting | D3DFVF_NORMAL))
	{
		definitions.push_back({ "RS_LIGHTING", "1" });
	}

	if (flags & ShaderFlags::rs_specular)
	{
		definitions.push_back({ "RS_SPECULAR", "1" });
	}

	if (flags & ShaderFlags::rs_alpha)
	{
		definitions.push_back({ "RS_ALPHA", "1" });
	}

	if (flags & ShaderFlags::rs_oit)
	{
		definitions.push_back({ "RS_OIT", "1" });
	}

	if (flags & ShaderFlags::rs_fog)
	{
		definitions.push_back({ "RS_FOG", "1" });
	}

	//shader_preproc_defs.push_back({});

	shader_preproc_definitions[flags] = std::move(definitions);
	return shader_preproc_definitions[flags];
}

void Direct3DDevice8::draw_call_increment()
{
	per_model.draw_call = (per_model.draw_call.data() + 1) % 65536;
}

static constexpr auto SHADER_COMPILER_FLAGS =
	D3DCOMPILE_PREFER_FLOW_CONTROL |
	D3DCOMPILE_DEBUG
#ifndef _DEBUG
	| D3DCOMPILE_OPTIMIZATION_LEVEL3
#endif
;

VertexShader Direct3DDevice8::get_vs(ShaderFlags::type flags, bool speedy_speed_boy,
                                     std::unordered_map<ShaderFlags::type, VertexShader>& shaders, std::recursive_mutex& mutex)
{
	flags = ShaderFlags::sanitize(flags & ShaderFlags::vs_mask);

	{
		std::lock_guard<std::recursive_mutex> lock(mutex);

		const auto it = shaders.find(flags);

		if (it != shaders.end())
		{
			return it->second;
		}
	}

	//printf(__FUNCTION__ " compiling vs: %04X (total: %u)\n", flags, vs_map.size() + 1);

	auto preproc = shader_preprocess(flags);

	if (speedy_speed_boy)
	{
		preproc.push_back({ "SPEEDY_SPEED_BOY", "1" });
	}
	
	preproc.push_back({});

	ComPtr<ID3DBlob> errors;
	ComPtr<ID3DBlob> blob;
	ComPtr<ID3D11VertexShader> shader;

	ShaderIncluder includer;

	constexpr auto path = "shader.hlsl";
	const auto& src = includer.get_shader_source(path);

	HRESULT hr = D3DCompile(src.data(), src.size(), path, preproc.data(), &includer, "vs_main", "vs_5_0", SHADER_COMPILER_FLAGS, 0, &blob, &errors);

	if (errors != nullptr)
	{
		const std::string str(static_cast<char*>(errors->GetBufferPointer()), 0, errors->GetBufferSize());

		OutputDebugStringA("\n" __FUNCTION__ "\n");
		OutputDebugStringA(str.c_str());
		OutputDebugStringA("\n");

		if (FAILED(hr))
		{
			throw std::runtime_error(str);
		}
	}

	hr = device->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &shader);

	if (FAILED(hr))
	{
		throw std::runtime_error("vertex shader creation failed");
	}


	{
		std::lock_guard<std::recursive_mutex> lock(mutex);

		auto result = VertexShader(shader, blob);
		shaders[flags] = result;

		LOCK(permutation_mutex);

		if (permutation_cache.is_open() && !permutation_flags.contains(flags))
		{
			OutputDebugStringA("writing vs permutation to permutation file\n");
			permutation_cache.write(reinterpret_cast<const char*>(&flags), sizeof(flags));
			permutation_cache.flush();
		}

		return result;
	}
}

PixelShader Direct3DDevice8::get_ps(ShaderFlags::type flags, bool speedy_speed_boy,
                                    std::unordered_map<ShaderFlags::type, PixelShader>& shaders, std::recursive_mutex& mutex)
{
	flags = ShaderFlags::sanitize(flags & ShaderFlags::ps_mask);

	{
		std::lock_guard<std::recursive_mutex> lock(mutex);

		const auto it = shaders.find(flags);

		if (it != shaders.end())
		{
			return it->second;
		}
	}

	//printf(__FUNCTION__ " compiling ps: %04X (total: %u)\n", flags, shaders.size() + 1);

	auto preproc = shader_preprocess(flags);

	if (speedy_speed_boy)
	{
		preproc.push_back({ "SPEEDY_SPEED_BOY", "1" });
	}
	
	preproc.push_back({});

	ComPtr<ID3DBlob> errors;
	ComPtr<ID3DBlob> blob;
	ComPtr<ID3D11PixelShader> shader;

	ShaderIncluder includer;

	constexpr auto path = "shader.hlsl";
	const auto& src = includer.get_shader_source(path);

	HRESULT hr = D3DCompile(src.data(), src.size(), path, preproc.data(), &includer, "ps_main", "ps_5_0", SHADER_COMPILER_FLAGS, 0, &blob, &errors);

	if (errors != nullptr)
	{
		const std::string str(static_cast<char*>(errors->GetBufferPointer()), 0, errors->GetBufferSize());

		OutputDebugStringA("\n" __FUNCTION__ "\n");
		OutputDebugStringA(str.c_str());
		OutputDebugStringA("\n");

		if (FAILED(hr))
		{
			throw std::runtime_error(str);
		}
	}

	hr = device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &shader);

	if (FAILED(hr))
	{
		throw std::runtime_error("pixel shader creation failed");
	}

	{
		std::lock_guard<std::recursive_mutex> lock(mutex);

		auto result = PixelShader(shader, blob);
		shaders[flags] = result;

		LOCK(permutation_mutex);

		if (permutation_cache.is_open() && !permutation_flags.contains(flags))
		{
			OutputDebugStringA("writing ps permutation to permutation file\n");
			permutation_cache.write(reinterpret_cast<const char*>(&flags), sizeof(flags));
			permutation_cache.flush();
		}

		return result;
	}
}

void Direct3DDevice8::create_depth_stencil()
{
	depth_stencil = new Direct3DTexture8(this, present_params.BackBufferWidth, present_params.BackBufferHeight, 1,
	                                     D3DUSAGE_DEPTHSTENCIL, present_params.AutoDepthStencilFormat, D3DPOOL_DEFAULT);

	depth_stencil->create_native();
	depth_stencil->GetSurfaceLevel(0, &current_depth_stencil);
}

#pragma comment( lib, "dxguid.lib")

void Direct3DDevice8::create_composite_texture(D3D11_TEXTURE2D_DESC& tex_desc)
{
	tex_desc.Usage     = D3D11_USAGE_DEFAULT;
	tex_desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

	HRESULT hr = device->CreateTexture2D(&tex_desc, nullptr, &composite_texture);

	if (FAILED(hr))
	{
		throw std::runtime_error("Failed to create composite target texture");
	}

	D3D11_RENDER_TARGET_VIEW_DESC view_desc {};

	view_desc.Format             = tex_desc.Format;
	view_desc.ViewDimension      = D3D11_RTV_DIMENSION_TEXTURE2D;
	view_desc.Texture2D.MipSlice = 0;

	hr = device->CreateRenderTargetView(composite_texture.Get(), &view_desc, &composite_view);

	if (FAILED(hr))
	{
		throw std::runtime_error("Failed to create composite target view");
	}

	std::string composite_view_name = "composite_view";
	composite_view->SetPrivateData(WKPDID_D3DDebugObjectName, composite_view_name.size(), composite_view_name.data());

	D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc {};

	srv_desc.Format                    = tex_desc.Format;
	srv_desc.ViewDimension             = D3D11_SRV_DIMENSION_TEXTURE2D;
	srv_desc.Texture2D.MostDetailedMip = 0;
	srv_desc.Texture2D.MipLevels       = 1;

	hr = device->CreateShaderResourceView(composite_texture.Get(), &srv_desc, &composite_srv);

	if (FAILED(hr))
	{
		throw std::runtime_error("Failed to create composite resource view");
	}

	std::string composite_srv_name = "composite_srv";
	composite_srv->SetPrivateData(WKPDID_D3DDebugObjectName, composite_srv_name.size(), composite_srv_name.data());

	composite_wrapper = new Direct3DTexture8(this, tex_desc.Width, tex_desc.Height, tex_desc.MipLevels, D3DUSAGE_RENDERTARGET, present_params.BackBufferFormat, D3DPOOL_DEFAULT);
	composite_wrapper->create_native(composite_texture.Get());
}

void Direct3DDevice8::create_render_target(D3D11_TEXTURE2D_DESC& tex_desc)
{
	tex_desc.Usage     = D3D11_USAGE_DEFAULT;
	tex_desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

	HRESULT hr = device->CreateTexture2D(&tex_desc, nullptr, &render_target_texture);

	if (FAILED(hr))
	{
		throw std::runtime_error("Failed to create render target texture");
	}

	D3D11_RENDER_TARGET_VIEW_DESC view_desc {};

	view_desc.Format             = tex_desc.Format;
	view_desc.ViewDimension      = D3D11_RTV_DIMENSION_TEXTURE2D;
	view_desc.Texture2D.MipSlice = 0;

	hr = device->CreateRenderTargetView(render_target_texture.Get(), &view_desc, &render_target_view);

	if (FAILED(hr))
	{
		throw std::runtime_error("Failed to create render target view");
	}

	std::string view_name = "render_target_view";
	render_target_view->SetPrivateData(WKPDID_D3DDebugObjectName, view_name.size(), view_name.data());

	D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc {};

	srv_desc.Format                    = tex_desc.Format;
	srv_desc.ViewDimension             = D3D11_SRV_DIMENSION_TEXTURE2D;
	srv_desc.Texture2D.MostDetailedMip = 0;
	srv_desc.Texture2D.MipLevels       = 1;

	hr = device->CreateShaderResourceView(render_target_texture.Get(), &srv_desc, &render_target_srv);

	if (FAILED(hr))
	{
		throw std::runtime_error("Failed to create composite resource view");
	}

	std::string render_target_srv_name = "render_target_srv";
	render_target_srv->SetPrivateData(WKPDID_D3DDebugObjectName, render_target_srv_name.size(), render_target_srv_name.data());

	render_target_wrapper = new Direct3DTexture8(this, tex_desc.Width, tex_desc.Height, tex_desc.MipLevels, D3DUSAGE_RENDERTARGET, present_params.BackBufferFormat, D3DPOOL_DEFAULT);
	render_target_wrapper->create_native(render_target_texture.Get());
}

void Direct3DDevice8::get_back_buffer()
{
	ID3D11Texture2D* pBackBuffer = nullptr;
	swap_chain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<LPVOID*>(&pBackBuffer));

	D3D11_TEXTURE2D_DESC tex_desc {};
	pBackBuffer->GetDesc(&tex_desc);

	back_buffer = new Direct3DTexture8(this, tex_desc.Width, tex_desc.Height, tex_desc.MipLevels, D3DUSAGE_RENDERTARGET, present_params.BackBufferFormat, D3DPOOL_DEFAULT);
	back_buffer->create_native(pBackBuffer);

	//back_buffer->GetSurfaceLevel(0, &current_render_target);

	device->CreateRenderTargetView(pBackBuffer, nullptr, &back_buffer_view);

	pBackBuffer->Release();

	ComPtr<Direct3DSurface8> ds_surface;
	depth_stencil->GetSurfaceLevel(0, &ds_surface);

	create_composite_texture(tex_desc);
	create_render_target(tex_desc);

	if (oit_enabled)
	{
		context->OMSetRenderTargets(1, composite_view.GetAddressOf(), ds_surface->depth_stencil.Get());

		// TODO: this doesn't make sense! If a program is expecting the render target with things in it, this has nothing until ::Present()!
		composite_wrapper->GetSurfaceLevel(0, &current_render_target);
	}
	else
	{
		// set the composite render target as the back buffer
		context->OMSetRenderTargets(1, render_target_view.GetAddressOf(), ds_surface->depth_stencil.Get());
		render_target_wrapper->GetSurfaceLevel(0, &current_render_target);
	}
}

void Direct3DDevice8::create_native()
{
	if (!std::filesystem::exists(d3d8to11::storage_directory))
	{
		std::filesystem::create_directory(d3d8to11::storage_directory);
	}

	if (std::filesystem::exists(d3d8to11::config_file_path))
	{
		std::fstream file(d3d8to11::config_file_path.string(), std::fstream::in);

		ini_file ini;
		ini.read(file);

		auto section = ini.get_section("OIT");

		if (section)
		{
			oit_enabled = section->get_or("enabled", false);
		}
	}
	else
	{
		std::fstream file(d3d8to11::config_file_path.string(), std::fstream::out);

		ini_file ini;

		auto section = std::make_shared<ini_section>();

		ini.set_section("OIT", section);
		section->set("enabled", oit_enabled);

		ini.write(file);
	}

	if (!present_params.EnableAutoDepthStencil)
	{
		throw std::runtime_error("manual depth buffer not supported");
	}

	palette_flag = supports_palettes();

	/*
	 * BackBufferWidth and BackBufferHeight 
	 * Width and height of the new swap chain's back buffers, in pixels. If Windowed is FALSE (the presentation is full-screen),
	 * then these values must equal the width and height of one of the enumerated display modes found through IDirect3D8::EnumAdapterModes.
	 * If Windowed is TRUE and either of these values is zero, then the corresponding dimension of the client area of the hDeviceWindow
	 * TODO: (or the focus window, if hDeviceWindow is NULL) is taken.
	 */

	UINT& width = present_params.BackBufferWidth;
	UINT& height = present_params.BackBufferHeight;

	if (present_params.Windowed && (!width || !height))
	{
		RECT rect;
		GetClientRect(present_params.hDeviceWindow, &rect);

		if (!width)
		{
			width = rect.right - rect.left;
		}

		if (!height)
		{
			height = rect.bottom - rect.top;
		}
	}

	DXGI_SWAP_CHAIN_DESC desc = {};

	desc.BufferCount        = 1;
	desc.BufferDesc.Format  = d3d8to11::to_dxgi(present_params.BackBufferFormat);
	desc.BufferUsage        = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	desc.BufferDesc.Width   = width;
	desc.BufferDesc.Height  = height;
	desc.BufferDesc.Scaling = DXGI_MODE_SCALING_CENTERED;
	desc.OutputWindow       = present_params.hDeviceWindow;
	desc.SampleDesc.Count   = 1;
	desc.Windowed           = present_params.Windowed;
	desc.Flags              = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;

	auto feature_level = static_cast<D3D_FEATURE_LEVEL>(0);

#ifdef _DEBUG
	constexpr auto flag = D3D11_CREATE_DEVICE_DEBUG;
#else
	constexpr auto flag = 0;
#endif

	// TODO: use more modern swap chain creation and management
	auto error = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flag,
	                                           FEATURE_LEVELS.data(), static_cast<UINT>(FEATURE_LEVELS.size()),
	                                           D3D11_SDK_VERSION, &desc, &swap_chain,
	                                           &device, &feature_level, &context);

	if (feature_level < D3D_FEATURE_LEVEL_11_0)
	{
		throw std::runtime_error("Device does not meet the minimum required feature level (D3D_FEATURE_LEVEL_11_0).");
	}

	if (error != S_OK)
	{
		throw std::runtime_error("Device creation failed with a known error that I'm too lazy to get the details of.");
	}

	device->QueryInterface(__uuidof(ID3D11InfoQueue), &info_queue);

	if (info_queue)
	{
		OutputDebugStringA("D3D11 debug info queue enabled\n");
		info_queue->SetMuteDebugOutput(FALSE);
	}

	swap_chain->SetFullscreenState(!present_params.Windowed, nullptr);

	create_depth_stencil();
	get_back_buffer();

	D3DVIEWPORT8 vp {};
	vp.Width  = present_params.BackBufferWidth;
	vp.Height = present_params.BackBufferHeight;
	vp.MaxZ   = 1.0f;
	SetViewport(&vp);
	
	HRESULT hr = make_cbuffer(per_scene, per_scene_cbuf);
	if (FAILED(hr))
	{
		throw std::runtime_error("per-scene CreateBuffer failed");
	}

	hr = make_cbuffer(per_model, per_model_cbuf);
	if (FAILED(hr))
	{
		throw std::runtime_error("per-model CreateBuffer failed");
	}

	hr = make_cbuffer(per_pixel, per_pixel_cbuf);
	if (FAILED(hr))
	{
		throw std::runtime_error("per-pixel CreateBuffer failed");
	}

	hr = make_cbuffer(per_texture, per_texture_cbuf);
	if (FAILED(hr))
	{
		throw std::runtime_error("per-texture CreateBuffer failed");
	}

	context->VSSetConstantBuffers(0, 1, per_scene_cbuf.GetAddressOf());
	context->PSSetConstantBuffers(0, 1, per_scene_cbuf.GetAddressOf());

	context->VSSetConstantBuffers(1, 1, per_model_cbuf.GetAddressOf());
	context->PSSetConstantBuffers(1, 1, per_model_cbuf.GetAddressOf());

	context->VSSetConstantBuffers(2, 1, per_pixel_cbuf.GetAddressOf());
	context->PSSetConstantBuffers(2, 1, per_pixel_cbuf.GetAddressOf());

	context->VSSetConstantBuffers(3, 1, per_texture_cbuf.GetAddressOf());
	context->PSSetConstantBuffers(3, 1, per_texture_cbuf.GetAddressOf());

	{
		bool exists = std::filesystem::exists(d3d8to11::permutation_file_path);

		std::fstream file;
		file.open(d3d8to11::permutation_file_path, std::ios::binary | std::ios::in | std::ios::out);

		if (!file.is_open())
		{
			file.open(d3d8to11::permutation_file_path, std::ios::binary | std::ios::in | std::ios::out | std::ios::trunc);

			if (!file.is_open())
			{
				OutputDebugStringA("fuck\n");
			}
		}

		if (exists)
		{
			OutputDebugStringA("precompiling shaders...\n");

			while (!file.eof())
			{
				ShaderFlags::type flags = 0;

				file.read(reinterpret_cast<char*>(&flags), sizeof(flags));

				{
					LOCK(permutation_mutex);
					permutation_flags.insert(flags);
				}

				std::stringstream ss;
				ss << "compiling: " << flags << "\n"; // because OutputDebugStringA doesn't like std::endl
				OutputDebugStringA(ss.str().c_str());

				//get_ps(flags & ShaderFlags::ps_mask, false, pixel_shaders, ps_mutex);
				//get_vs(flags & ShaderFlags::vs_mask, false, vertex_shaders, vs_mutex);

				VertexShader vs_dummy;
				PixelShader ps_dummy;

				compile_shaders(flags, vs_dummy, ps_dummy);
			}

			OutputDebugStringA("done\n");
		}

		file.seekg(0, std::ios_base::end);
		file.seekp(0, std::ios_base::end);
		permutation_cache = std::move(file);
	}

	blend_flags        = 0;
	raster_flags       = 0;
	depthstencil_flags = {};

	// TODO: properly set default for D3DRS_ZENABLE; see below
	// The default value for this render state is D3DZB_TRUE if a depth stencil was created along with the swap chain by setting
	// the EnableAutoDepthStencil member of the D3DPRESENT_PARAMETERS structure to TRUE, and D3DZB_FALSE otherwise. 
	SetRenderState(D3DRS_ZENABLE, TRUE);

	SetRenderState(D3DRS_ZWRITEENABLE,     TRUE);
	SetRenderState(D3DRS_FILLMODE,         D3DFILL_SOLID);
	SetRenderState(D3DRS_SRCBLEND,         D3DBLEND_ONE);
	SetRenderState(D3DRS_DESTBLEND,        D3DBLEND_ZERO);
	SetRenderState(D3DRS_CULLMODE,         D3DCULL_CCW);
	SetRenderState(D3DRS_ZFUNC,            D3DCMP_LESS);
	SetRenderState(D3DRS_ALPHAFUNC,        D3DCMP_ALWAYS);
	SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
	SetRenderState(D3DRS_FOGENABLE,        FALSE);
	SetRenderState(D3DRS_SPECULARENABLE,   TRUE);
	SetRenderState(D3DRS_LIGHTING,         TRUE);
	SetRenderState(D3DRS_COLORVERTEX,      TRUE);
	SetRenderState(D3DRS_LOCALVIEWER,      TRUE);
	SetRenderState(D3DRS_BLENDOP,          D3DBLENDOP_ADD);
	SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);

	SetRenderState(D3DRS_AMBIENTMATERIALSOURCE,  D3DMCS_MATERIAL);
	SetRenderState(D3DRS_DIFFUSEMATERIALSOURCE,  D3DMCS_COLOR1);
	SetRenderState(D3DRS_SPECULARMATERIALSOURCE, D3DMCS_COLOR2);
	SetRenderState(D3DRS_EMISSIVEMATERIALSOURCE, D3DMCS_MATERIAL);

	SetRenderState(D3DRS_STENCILENABLE,    FALSE);
	SetRenderState(D3DRS_STENCILFAIL,      D3DSTENCILOP_KEEP);
	SetRenderState(D3DRS_STENCILZFAIL,     D3DSTENCILOP_KEEP);
	SetRenderState(D3DRS_STENCILPASS,      D3DSTENCILOP_KEEP);
	SetRenderState(D3DRS_STENCILFUNC,      D3DCMP_ALWAYS);
	SetRenderState(D3DRS_STENCILREF,       0);
	SetRenderState(D3DRS_STENCILMASK,      0xFFFFFFFF);
	SetRenderState(D3DRS_STENCILWRITEMASK, 0xFFFFFFFF);

	for (auto& state : render_state_values)
	{
		state.mark();
	}

	// set all the texture stage states to their defaults

	for (size_t i = 0; i < TEXTURE_STAGE_MAX; i++)
	{
		SetTextureStageState(i, D3DTSS_COLOROP, !i ? D3DTOP_MODULATE : D3DTOP_DISABLE);
		SetTextureStageState(i, D3DTSS_COLORARG1, D3DTA_TEXTURE);
		SetTextureStageState(i, D3DTSS_COLORARG2, D3DTA_CURRENT);
		SetTextureStageState(i, D3DTSS_ALPHAOP, !i ? D3DTOP_SELECTARG1 : D3DTOP_DISABLE);
		SetTextureStageState(i, D3DTSS_ALPHAARG1, D3DTA_TEXTURE); // If no texture is set for this stage, the default argument is D3DTA_DIFFUSE.
		SetTextureStageState(i, D3DTSS_ALPHAARG2, D3DTA_CURRENT);
		SetTextureStageState(i, D3DTSS_BUMPENVMAT00, 0);
		SetTextureStageState(i, D3DTSS_BUMPENVMAT01, 0);
		SetTextureStageState(i, D3DTSS_BUMPENVMAT10, 0);
		SetTextureStageState(i, D3DTSS_BUMPENVMAT11, 0);
		SetTextureStageState(i, D3DTSS_TEXCOORDINDEX, i);
		SetTextureStageState(i, D3DTSS_ADDRESSU, D3DTADDRESS_WRAP);
		SetTextureStageState(i, D3DTSS_ADDRESSV, D3DTADDRESS_WRAP);
		SetTextureStageState(i, D3DTSS_BORDERCOLOR, 0);
		SetTextureStageState(i, D3DTSS_MAGFILTER, D3DTEXF_POINT);
		SetTextureStageState(i, D3DTSS_MINFILTER, D3DTEXF_POINT);
		SetTextureStageState(i, D3DTSS_MIPFILTER, D3DTEXF_NONE);
		SetTextureStageState(i, D3DTSS_MIPMAPLODBIAS, 0);
		SetTextureStageState(i, D3DTSS_MAXMIPLEVEL, 0);
		SetTextureStageState(i, D3DTSS_MAXANISOTROPY, 1);
		SetTextureStageState(i, D3DTSS_BUMPENVLSCALE, 0);
		SetTextureStageState(i, D3DTSS_BUMPENVLOFFSET, 0);
		SetTextureStageState(i, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
		SetTextureStageState(i, D3DTSS_ADDRESSW, D3DTADDRESS_WRAP);
		SetTextureStageState(i, D3DTSS_COLORARG0, D3DTA_CURRENT);
		SetTextureStageState(i, D3DTSS_ALPHAARG0, D3DTA_CURRENT);
		SetTextureStageState(i, D3DTSS_RESULTARG, D3DTA_CURRENT);
	}

	D3DMATERIAL8 material {};
	material.Diffuse = { 1.0f, 1.0f, 1.0f, 1.0f };
	SetMaterial(&material);

	blend_flags.mark();
	raster_flags.mark();
	depthstencil_flags.mark();

	FVF = 0;
	FVF.mark();

	per_scene.mark();
	per_model.mark();
	per_pixel.mark();
	per_texture.mark();

	oit_load_shaders();
	oit_init();

	update();
}

ShaderFlags::type ShaderFlags::sanitize(type flags)
{
	flags &= mask;

	if (flags & rs_lighting && !(flags & D3DFVF_NORMAL))
	{
		flags &= ~rs_lighting;
	}

	if (flags & rs_oit && !(flags & rs_alpha))
	{
		flags &= ~rs_oit;
	}

	return flags;
}

bool DepthStencilFlags::dirty() const
{
	return flags.dirty() || depth_flags.dirty() || stencil_flags.dirty();
}

void DepthStencilFlags::clear()
{
	flags.clear();
	depth_flags.clear();
	stencil_flags.clear();
}

void DepthStencilFlags::mark()
{
	flags.mark();
	depth_flags.mark();
	stencil_flags.mark();
}

bool DepthStencilFlags::operator==(const DepthStencilFlags& rhs) const
{
	return flags.data()         == rhs.flags.data() &&
	       depth_flags.data()   == rhs.depth_flags.data() &&
	       stencil_flags.data() == rhs.stencil_flags.data();
}

SamplerSettings::SamplerSettings()
{
	address_u      = D3DTADDRESS_WRAP;
	address_v      = D3DTADDRESS_WRAP;
	address_w      = D3DTADDRESS_WRAP;
	filter_mag     = D3DTEXF_POINT;
	filter_min     = D3DTEXF_POINT;
	filter_mip     = D3DTEXF_NONE;
	mip_lod_bias   = 0.0f;
	max_mip_level  = 0;
	max_anisotropy = 1;
}

bool SamplerSettings::operator==(const SamplerSettings& s) const
{
	return address_u.data()      == s.address_u.data() &&
	       address_v.data()      == s.address_v.data() &&
	       address_w.data()      == s.address_w.data() &&
	       filter_mag.data()     == s.filter_mag.data() &&
	       filter_min.data()     == s.filter_min.data() &&
	       filter_mip.data()     == s.filter_mip.data() &&
	       mip_lod_bias.data()   == s.mip_lod_bias.data() &&
	       max_mip_level.data()  == s.max_mip_level.data() &&
	       max_anisotropy.data() == s.max_anisotropy.data();
}

bool SamplerSettings::dirty() const
{
	return address_u.dirty() ||
	       address_v.dirty() ||
	       address_w.dirty() ||
	       filter_mag.dirty() ||
	       filter_min.dirty() ||
	       filter_mip.dirty() ||
	       mip_lod_bias.dirty() ||
	       max_mip_level.dirty() ||
	       max_anisotropy.dirty();
}

void SamplerSettings::clear()
{
	address_u.clear();
	address_v.clear();
	address_w.clear();
	filter_mag.clear();
	filter_min.clear();
	filter_mip.clear();
	mip_lod_bias.clear();
	max_mip_level.clear();
	max_anisotropy.clear();
}

void SamplerSettings::mark()
{
	address_u.mark();
	address_v.mark();
	address_w.mark();
	filter_mag.mark();
	filter_min.mark();
	filter_mip.mark();
	mip_lod_bias.mark();
	max_mip_level.mark();
	max_anisotropy.mark();
}

// IDirect3DDevice8
Direct3DDevice8::Direct3DDevice8(Direct3D8* d3d, const D3DPRESENT_PARAMETERS8& parameters)
	: present_params(parameters),
	  d3d(d3d)
{
	fragments_str = std::to_string(globals::max_fragments);
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::QueryInterface(REFIID riid, void** ppvObj)
{
	if (ppvObj == nullptr)
	{
		return E_POINTER;
	}

	if (riid == __uuidof(this) ||
	    riid == __uuidof(IUnknown))
	{
		AddRef();

		*ppvObj = this;

		return S_OK;
	}

	return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE Direct3DDevice8::AddRef()
{
	return Unknown::AddRef();
}

ULONG STDMETHODCALLTYPE Direct3DDevice8::Release()
{
	ULONG LastRefCount = Unknown::Release();

	if (LastRefCount == 0)
	{
		delete this;
	}

	return LastRefCount;
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::TestCooperativeLevel()
{
	// TODO: TestCooperativeLevel
	return D3DERR_INVALIDCALL;
}

UINT STDMETHODCALLTYPE Direct3DDevice8::GetAvailableTextureMem()
{
#if 1
	return UINT_MAX;
#else
	return ProxyInterface->GetAvailableTextureMem();
#endif
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::ResourceManagerDiscardBytes(DWORD Bytes)
{
	UNREFERENCED_PARAMETER(Bytes);

#if 1
	// not yet supported
	return D3DERR_INVALIDCALL;
#else
	return ProxyInterface->EvictManagedResources();
#endif
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::GetDirect3D(Direct3D8** ppD3D8)
{
	if (ppD3D8 == nullptr)
	{
		return D3DERR_INVALIDCALL;
	}

	d3d->AddRef();

	*ppD3D8 = d3d;

	return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::GetDeviceCaps(D3DCAPS8* pCaps)
{
	if (!pCaps)
	{
		return D3DERR_INVALIDCALL;
	}

	// TODO: properly populate pCaps
	*pCaps = {};

	pCaps->MaxTextureWidth  = D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION;
	pCaps->MaxTextureHeight = D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION;

	pCaps->Caps2                    = 0xFFFFFFFF;
	pCaps->Caps3                    = 0xFFFFFFFF;
	pCaps->PresentationIntervals    = 0xFFFFFFFF;
	pCaps->DevCaps                  = 0xFFFFFFFF;
	pCaps->PrimitiveMiscCaps        = 0xFFFFFFFF;
	pCaps->RasterCaps               = 0xFFFFFFFF;
	pCaps->ZCmpCaps                 = 0xFFFFFFFF;
	pCaps->SrcBlendCaps             = 0xFFFFFFFF;
	pCaps->DestBlendCaps            = 0xFFFFFFFF;
	pCaps->AlphaCmpCaps             = 0xFFFFFFFF;
	pCaps->ShadeCaps                = 0xFFFFFFFF;
	pCaps->TextureCaps              = D3DPTEXTURECAPS_MIPMAP | D3DPTEXTURECAPS_ALPHA | D3DPTEXTURECAPS_NONPOW2CONDITIONAL | D3DPTEXTURECAPS_PROJECTED;
	pCaps->TextureFilterCaps        = 0xFFFFFFFF;
	pCaps->CubeTextureFilterCaps    = 0xFFFFFFFF;
	pCaps->VolumeTextureFilterCaps  = 0xFFFFFFFF;
	pCaps->TextureAddressCaps       = 0xFFFFFFFF;
	pCaps->VolumeTextureAddressCaps = 0xFFFFFFFF;
	pCaps->LineCaps                 = 0xFFFFFFFF;
	pCaps->MaxTextureRepeat         = 0xFFFFFFFF;
	pCaps->MaxTextureAspectRatio    = 0xFFFFFFFF;
	pCaps->MaxAnisotropy            = 16;
	pCaps->StencilCaps              = 0xFFFFFFFF;
	pCaps->FVFCaps                  = 0xFFFFFFFF;
	pCaps->TextureOpCaps            = 0xFFFFFFFF;
	pCaps->MaxActiveLights          = LIGHT_COUNT;

	pCaps->MaxTextureBlendStages   = TEXTURE_STAGE_MAX;
	pCaps->MaxSimultaneousTextures = TEXTURE_STAGE_MAX;

	return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::GetDisplayMode(D3DDISPLAYMODE* pMode)
{
#if 1
	// not yet supported
	return D3DERR_INVALIDCALL;
#else
	return ProxyInterface->GetDisplayMode(0, pMode);
#endif
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::GetCreationParameters(D3DDEVICE_CREATION_PARAMETERS* pParameters)
{
#if 1
	// not yet supported
	return D3DERR_INVALIDCALL;
#else
	return ProxyInterface->GetCreationParameters(pParameters);
#endif
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::SetCursorProperties(UINT XHotSpot, UINT YHotSpot, Direct3DSurface8* pCursorBitmap)
{
#if 1
	// not yet supported
	return D3DERR_INVALIDCALL;
#else
	if (pCursorBitmap == nullptr)
	{
		return D3DERR_INVALIDCALL;
	}

	return ProxyInterface->SetCursorProperties(XHotSpot, YHotSpot, pCursorBitmap->GetProxyInterface());
#endif
}

void STDMETHODCALLTYPE Direct3DDevice8::SetCursorPosition(UINT XScreenSpace, UINT YScreenSpace, DWORD Flags)
{
	// not yet supported
#if 0
	ProxyInterface->SetCursorPosition(XScreenSpace, YScreenSpace, Flags);
#endif
}

BOOL STDMETHODCALLTYPE Direct3DDevice8::ShowCursor(BOOL bShow)
{
#if 1
	// not yet supported
	return FALSE;
#else
	return ProxyInterface->ShowCursor(bShow);
#endif
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::CreateAdditionalSwapChain(D3DPRESENT_PARAMETERS8* pPresentationParameters, Direct3DSwapChain8** ppSwapChain)
{
#if 1
	// not yet supported
	return D3DERR_INVALIDCALL;
#else
	if (pPresentationParameters == nullptr || ppSwapChain == nullptr)
	{
		return D3DERR_INVALIDCALL;
	}

	*ppSwapChain = nullptr;

	D3DPRESENT_PARAMETERS PresentParams;
	ConvertPresentParameters(*pPresentationParameters, PresentParams);

	// Get multisample quality level
	if (PresentParams.MultiSampleType != D3DMULTISAMPLE_NONE)
	{
		DWORD QualityLevels = 0;
		D3DDEVICE_CREATION_PARAMETERS CreationParams;
		ProxyInterface->GetCreationParameters(&CreationParams);

		if (d3d->GetProxyInterface()->CheckDeviceMultiSampleType(CreationParams.AdapterOrdinal,
			CreationParams.DeviceType, PresentParams.BackBufferFormat, PresentParams.Windowed,
			PresentParams.MultiSampleType, &QualityLevels) == S_OK &&
			d3d->GetProxyInterface()->CheckDeviceMultiSampleType(CreationParams.AdapterOrdinal,
				CreationParams.DeviceType, PresentParams.AutoDepthStencilFormat, PresentParams.Windowed,
				PresentParams.MultiSampleType, &QualityLevels) == S_OK)
		{
			PresentParams.MultiSampleQuality = (QualityLevels != 0) ? QualityLevels - 1 : 0;
		}
	}

	IDirect3DSwapChain9 *SwapChainInterface = nullptr;

	const HRESULT hr = ProxyInterface->CreateAdditionalSwapChain(&PresentParams, &SwapChainInterface);

	if (FAILED(hr))
	{
		return hr;
	}

	*ppSwapChain = new Direct3DSwapChain8(this, SwapChainInterface);
	(*ppSwapChain)->AddRef();

	return D3D_OK;
#endif
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::Reset(D3DPRESENT_PARAMETERS8* pPresentationParameters)
{
	if (!pPresentationParameters)
	{
		return D3DERR_INVALIDCALL;
	}

	// TODO: handle actual device lost state
	// TODO: handle/fix fullscreen toggle

	if (pPresentationParameters->BackBufferWidth != present_params.BackBufferWidth ||
	    pPresentationParameters->BackBufferHeight != present_params.BackBufferHeight ||
	    pPresentationParameters->Windowed != present_params.Windowed)
	{
		present_params = *pPresentationParameters;

		context->OMSetRenderTargets(0, nullptr, nullptr);

		back_buffer           = nullptr;
		current_depth_stencil = nullptr;
		current_render_target = nullptr;
		back_buffer_view      = nullptr;

		swap_chain->ResizeBuffers(1, present_params.BackBufferWidth, present_params.BackBufferHeight,
		                          DXGI_FORMAT_UNKNOWN, DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH);

		create_depth_stencil();
		get_back_buffer();

		D3DVIEWPORT8 vp {};
		GetViewport(&vp);

		vp.Width  = present_params.BackBufferWidth;
		vp.Height = present_params.BackBufferHeight;

		SetViewport(&vp);

		oit_release();
		oit_init();

		shader_flags &= ~ShaderFlags::fvf_mask;
		FVF = 0;
		FVF.clear();
	}

	return D3D_OK;
}

void Direct3DDevice8::oit_composite()
{
	if (!oit_actually_enabled)
	{
		return;
	}

	DWORD CULLMODE, ZENABLE;
	GetRenderState(D3DRS_CULLMODE, &CULLMODE);
	GetRenderState(D3DRS_ZENABLE, &ZENABLE);

	SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
	SetRenderState(D3DRS_ZENABLE, FALSE);

	static constexpr auto BLEND_DEFAULT = D3DBLEND_ONE | (D3DBLEND_ONE << 4) | (D3DBLENDOP_ADD << 8) | (0xF << BLEND_COLORMASK_SHIFT);
	auto blend_ = blend_flags.data();
	blend_flags = BLEND_DEFAULT;
	update();

	// Unbinds UAV read/write buffers and binds their read-only
	// shader resource views.
	oit_read();

	ID3D11VertexShader* vs;
	ID3D11PixelShader* ps;

	context->VSGetShader(&vs, nullptr, nullptr);
	context->PSGetShader(&ps, nullptr, nullptr);

	// Switches to the composite to begin the sorting process.
	context->VSSetShader(composite_vs.shader.Get(), nullptr, 0);
	context->PSSetShader(composite_ps.shader.Get(), nullptr, 0);

	// Unbind the last vertex & index buffers
	context->IASetVertexBuffers(0, 0, nullptr, nullptr, nullptr);
	context->IASetIndexBuffer(nullptr, DXGI_FORMAT_UNKNOWN, 0);
	context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

	// ...then draw 3 points. The composite shader uses SV_VertexID
	// to generate a full screen triangle, so we don't need a buffer!
	context->Draw(3, 0);

	context->VSSetShader(vs, nullptr, 0);
	context->PSSetShader(ps, nullptr, 0);

	safe_release(&vs);
	safe_release(&ps);

	blend_flags = blend_;
	SetRenderState(D3DRS_CULLMODE, CULLMODE);
	SetRenderState(D3DRS_ZENABLE, ZENABLE);
	update();

	for (auto& stream : stream_sources)
	{
		SetStreamSource(stream.first, stream.second.buffer, stream.second.stride);
	}

	auto temp = std::move(index_buffer);
	SetIndices(temp.Get(), current_base_vertex_index);
}

void Direct3DDevice8::oit_start()
{
	if (!oit_enabled && oit_actually_enabled != oit_enabled)
	{
		oit_actually_enabled = oit_enabled;
		oit_write();
	}

	oit_actually_enabled = oit_enabled;

	if (oit_actually_enabled)
	{
		// Restore R/W access to the UAV buffers from the shader.
		oit_write();
	}
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::Present(const RECT* pSourceRect, const RECT* pDestRect, HWND hDestWindowOverride, const RGNDATA* pDirtyRegion)
{
	{
		ComPtr<Direct3DSurface8> rt_surface;
		render_target_wrapper->GetSurfaceLevel(0, &rt_surface);

		ComPtr<Direct3DSurface8> bb_surface;
		back_buffer->GetSurfaceLevel(0, &bb_surface);

		CopyRects(rt_surface.Get(), nullptr, 0, bb_surface.Get(), nullptr);
	}

	print_info_queue();
	UNREFERENCED_PARAMETER(pDirtyRegion);

	auto interval = present_params.FullScreen_PresentationInterval;

	if (interval == D3DPRESENT_INTERVAL_IMMEDIATE)
	{
		interval = 0;
	}

	oit_composite();

	per_model.draw_call = 0;

	try
	{
		if (FAILED(swap_chain->Present(interval, 0)))
		{
			return D3DERR_INVALIDCALL;
		}
	}
	catch (std::exception&)
	{
	}

	oit_start();

	const auto vk_shift   = GetAsyncKeyState(VK_SHIFT) & (1 << 16);
	const auto vk_control = GetAsyncKeyState(VK_CONTROL) & (1 << 16);
	const auto vk_o       = GetAsyncKeyState('O') & 1;
	const auto vk_r       = GetAsyncKeyState('R') & 1;

	if (vk_control && vk_o)
	{
		if (oit_actually_enabled == oit_enabled)
		{
			oit_enabled = !oit_enabled;
			OutputDebugStringA(oit_enabled ? "OIT enabled\n" : "OIT disabled\n");
		}
	}

	if (vk_control && vk_r)
	{
		if (!this->freeing_shaders)
		{
			OutputDebugStringA("clearing cached shaders...\n");

			if (vk_shift)
			{
				oit_load_shaders();
			}
			else
			{
				free_shaders();
				oit_load_shaders();
			}

			update();
			this->freeing_shaders = true;
		}
	}
	else
	{
		this->freeing_shaders = false;
	}

	return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::GetBackBuffer(UINT iBackBuffer, D3DBACKBUFFER_TYPE Type, Direct3DSurface8** ppBackBuffer)
{
	if (ppBackBuffer == nullptr)
	{
		return D3DERR_INVALIDCALL;
	}

	if (iBackBuffer) // TODO: actual backbuffer, actual swapchain
	{
		return D3DERR_INVALIDCALL; // HACK
	}

#if 0
	auto& bb = back_buffer;
#else
	auto& bb = render_target_wrapper;
#endif

	ComPtr<Direct3DSurface8> surface;
	bb->GetSurfaceLevel(0, &surface);

	*ppBackBuffer = surface.Get();
	(*ppBackBuffer)->AddRef();

	return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::GetRasterStatus(D3DRASTER_STATUS* pRasterStatus)
{
#if 1
	// not yet supported
	return D3DERR_INVALIDCALL;
#else
	return ProxyInterface->GetRasterStatus(0, pRasterStatus);
#endif
}

void STDMETHODCALLTYPE Direct3DDevice8::SetGammaRamp(DWORD Flags, const D3DGAMMARAMP* pRamp)
{
#if 1
	// not yet supported
	return;
#else
	ProxyInterface->SetGammaRamp(0, Flags, pRamp);
#endif
}

void STDMETHODCALLTYPE Direct3DDevice8::GetGammaRamp(D3DGAMMARAMP* pRamp)
{
#if 1
	// not yet supported
	return;
#else
	ProxyInterface->GetGammaRamp(0, pRamp);
#endif
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::CreateTexture(UINT Width, UINT Height, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, Direct3DTexture8** ppTexture)
{
	if (ppTexture == nullptr)
	{
		return D3DERR_INVALIDCALL;
	}

	*ppTexture = nullptr;

	if (Pool == D3DPOOL_DEFAULT)
	{
	#if 0
		D3DDEVICE_CREATION_PARAMETERS CreationParams;
		ProxyInterface->GetCreationParameters(&CreationParams);

		if ((Usage & D3DUSAGE_DYNAMIC) == 0 &&
			SUCCEEDED(d3d->GetProxyInterface()->CheckDeviceFormat(CreationParams.AdapterOrdinal, CreationParams.DeviceType, D3DFMT_X8R8G8B8, D3DUSAGE_RENDERTARGET, D3DRTYPE_TEXTURE, Format)))
		{
			Usage |= D3DUSAGE_RENDERTARGET;
		}
		else
	#endif
		{
			Usage |= D3DUSAGE_DYNAMIC;
		}
	}

	auto result = new Direct3DTexture8(this, Width, Height, Levels, Usage, Format, Pool);
	result->AddRef();

	try
	{
		result->create_native();
		*ppTexture = result;
	}
	catch (std::exception& ex)
	{
		delete result;

		std::string str = __FUNCTION__;
		str.append(" ");
		str.append(ex.what());

		OutputDebugStringA(str.c_str());

		print_info_queue();
		return D3DERR_INVALIDCALL;
	}

	return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::CreateVolumeTexture(UINT Width, UINT Height, UINT Depth, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, Direct3DVolumeTexture8** ppVolumeTexture)
{
#if 1
	// not yet supported
	return D3DERR_INVALIDCALL;
#else
	if (ppVolumeTexture == nullptr)
	{
		return D3DERR_INVALIDCALL;
	}

	*ppVolumeTexture = nullptr;

	IDirect3DVolumeTexture9 *TextureInterface = nullptr;

	const HRESULT hr = ProxyInterface->CreateVolumeTexture(Width, Height, Depth, Levels, Usage, Format, Pool, &TextureInterface, nullptr);

	if (FAILED(hr))
	{
		return hr;
	}

	*ppVolumeTexture = new Direct3DVolumeTexture8(this, TextureInterface);
	(*ppVolumeTexture)->AddRef();

	return D3D_OK;
#endif
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::CreateCubeTexture(UINT EdgeLength, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, Direct3DCubeTexture8** ppCubeTexture)
{
#if 1
	// not yet supported
	return D3DERR_INVALIDCALL;
#else
	if (ppCubeTexture == nullptr)
	{
		return D3DERR_INVALIDCALL;
	}

	*ppCubeTexture = nullptr;

	IDirect3DCubeTexture9 *TextureInterface = nullptr;

	const HRESULT hr = ProxyInterface->CreateCubeTexture(EdgeLength, Levels, Usage, Format, Pool, &TextureInterface, nullptr);

	if (FAILED(hr))
	{
		return hr;
	}

	*ppCubeTexture = new Direct3DCubeTexture8(this, TextureInterface);
	(*ppCubeTexture)->AddRef();

	return D3D_OK;
#endif
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::CreateVertexBuffer(UINT Length, DWORD Usage, DWORD FVF, D3DPOOL Pool, Direct3DVertexBuffer8** ppVertexBuffer)
{
	if (ppVertexBuffer == nullptr)
	{
		return D3DERR_INVALIDCALL;
	}

	*ppVertexBuffer = nullptr;
	auto result = new Direct3DVertexBuffer8(this, Length, Usage, FVF, Pool);
	result->AddRef();

	try
	{
		result->create_native();
		*ppVertexBuffer = result;
	}
	catch (std::exception& ex)
	{
		delete result;

		std::string str = __FUNCTION__;
		str.append(" ");
		str.append(ex.what());

		OutputDebugStringA(str.c_str());

		print_info_queue();
		return D3DERR_INVALIDCALL;
	}

	return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::CreateIndexBuffer(UINT Length, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, Direct3DIndexBuffer8** ppIndexBuffer)
{
	if (ppIndexBuffer == nullptr)
	{
		return D3DERR_INVALIDCALL;
	}

	*ppIndexBuffer = nullptr;
	auto result = new Direct3DIndexBuffer8(this, Length, Usage, Format, Pool);
	result->AddRef();

	try
	{
		result->create_native();
		*ppIndexBuffer = result;
	}
	catch (std::exception& ex)
	{
		delete result;

		std::string str = __FUNCTION__;
		str.append(" ");
		str.append(ex.what());

		OutputDebugStringA(str.c_str());

		print_info_queue();
		return D3DERR_INVALIDCALL;
	}

	return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::CreateRenderTarget(UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample, BOOL Lockable, Direct3DSurface8** ppSurface)
{
#if 1
	// not yet supported
	return D3DERR_INVALIDCALL;
#else
	if (ppSurface == nullptr)
	{
		return D3DERR_INVALIDCALL;
	}

	*ppSurface = nullptr;

	DWORD QualityLevels = 0;

	// Get multisample quality level
	if (MultiSample != D3DMULTISAMPLE_NONE)
	{
		D3DDEVICE_CREATION_PARAMETERS CreationParams;
		ProxyInterface->GetCreationParameters(&CreationParams);

		d3d->GetProxyInterface()->CheckDeviceMultiSampleType(CreationParams.AdapterOrdinal, CreationParams.DeviceType, Format, FALSE, MultiSample, &QualityLevels);
		QualityLevels = (QualityLevels != 0) ? QualityLevels - 1 : 0;
	}

	IDirect3DSurface9 *SurfaceInterface = nullptr;

	HRESULT hr = ProxyInterface->CreateRenderTarget(Width, Height, Format, MultiSample, QualityLevels, Lockable, &SurfaceInterface, nullptr);

	if (FAILED(hr))
	{
		return hr;
	}

	*ppSurface = new Direct3DSurface8(this, SurfaceInterface);
	(*ppSurface)->AddRef();

	return D3D_OK;
#endif
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::CreateDepthStencilSurface(UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample, Direct3DSurface8** ppSurface)
{
#if 1
	// not yet supported
	return D3DERR_INVALIDCALL;
#else
	if (ppSurface == nullptr)
	{
		return D3DERR_INVALIDCALL;
	}

	*ppSurface = nullptr;

	DWORD QualityLevels = 0;

	// Get multisample quality level
	if (MultiSample != D3DMULTISAMPLE_NONE)
	{
		D3DDEVICE_CREATION_PARAMETERS CreationParams;
		ProxyInterface->GetCreationParameters(&CreationParams);

		d3d->GetProxyInterface()->CheckDeviceMultiSampleType(CreationParams.AdapterOrdinal, CreationParams.DeviceType, Format, FALSE, MultiSample, &QualityLevels);
		QualityLevels = (QualityLevels != 0) ? QualityLevels - 1 : 0;
	}

	IDirect3DSurface9 *SurfaceInterface = nullptr;

	HRESULT hr = ProxyInterface->CreateDepthStencilSurface(Width, Height, Format, MultiSample, QualityLevels, ZBufferDiscarding, &SurfaceInterface, nullptr);

	if (FAILED(hr))
	{
		return hr;
	}

	*ppSurface = new Direct3DSurface8(this, SurfaceInterface);
	(*ppSurface)->AddRef();

	return D3D_OK;
#endif
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::CreateImageSurface(UINT Width, UINT Height, D3DFORMAT Format, Direct3DSurface8** ppSurface)
{
#if 1
	// not yet supported
	return D3DERR_INVALIDCALL;
#else
	if (ppSurface == nullptr)
	{
		return D3DERR_INVALIDCALL;
	}

	*ppSurface = nullptr;

	if (Format == D3DFMT_R8G8B8)
	{
		Format = D3DFMT_X8R8G8B8;
	}

	IDirect3DSurface9 *SurfaceInterface = nullptr;

	const HRESULT hr = ProxyInterface->CreateOffscreenPlainSurface(Width, Height, Format, D3DPOOL_SYSTEMMEM, &SurfaceInterface, nullptr);

	if (FAILED(hr))
	{
		return hr;
	}

	*ppSurface = new Direct3DSurface8(this, SurfaceInterface);
	(*ppSurface)->AddRef();

	return D3D_OK;
#endif
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::CopyRects(Direct3DSurface8* pSourceSurface, const RECT* pSourceRectsArray, UINT cRects,
                                                     Direct3DSurface8* pDestinationSurface, const POINT* pDestPointsArray)
{
	print_info_queue();

	if (!pSourceSurface || !pDestinationSurface)
	{
		return D3DERR_INVALIDCALL;
	}

	// HACK: all so I can avoid writing the code to copy subregions; they're so rarely used anyway
	// fixes this call in Phantasy Star Online: Blue Burst
	if (pSourceRectsArray && cRects == 1)
	{
		if (pSourceRectsArray->top || pSourceRectsArray->left)
		{
			return D3DERR_INVALIDCALL;
		}

		if (pSourceSurface->desc8.Width  != static_cast<uint>(pSourceRectsArray->right) ||
		    pSourceSurface->desc8.Height != static_cast<uint>(pSourceRectsArray->bottom))
		{
			return D3DERR_INVALIDCALL;
		}

		if (pDestPointsArray && (pDestPointsArray->x != 0 || pDestPointsArray->y != 0))
		{
			return D3DERR_INVALIDCALL;
		}
	}
	else if (pSourceRectsArray || cRects || pDestPointsArray) // TODO
	{
		return D3DERR_INVALIDCALL;
	}

	UINT src_index = 0;
	UINT dst_index = 0;
	ComPtr<ID3D11Resource> src, dst;

	// TODO: MAKE NOT SHIT

	if (pSourceSurface->render_target)
	{
		pSourceSurface->render_target->GetResource(&src);
	}
	else if (pSourceSurface->parent)
	{
		src = pSourceSurface->parent->texture;
		src_index = pSourceSurface->level;
	}

	if (pDestinationSurface->render_target)
	{
		pDestinationSurface->render_target->GetResource(&dst);
	}
	else if (pDestinationSurface->parent)
	{
		dst = pDestinationSurface->parent->texture;
		dst_index = pDestinationSurface->level;
	}

	context->CopySubresourceRegion(dst.Get(), dst_index, 0, 0, 0, src.Get(), src_index, nullptr);
	print_info_queue();
	return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::UpdateTexture(Direct3DBaseTexture8* pSourceTexture, Direct3DBaseTexture8* pDestinationTexture)
{
#if 1
	// not yet supported
	return D3DERR_INVALIDCALL;
#else
	if (pSourceTexture == nullptr || pDestinationTexture == nullptr || pSourceTexture->GetType() != pDestinationTexture->GetType())
	{
		return D3DERR_INVALIDCALL;
	}

	IDirect3DBaseTexture9 *SourceBaseTextureInterface, *DestinationBaseTextureInterface;

	switch (pSourceTexture->GetType())
	{
		case D3DRTYPE_TEXTURE:
			SourceBaseTextureInterface = static_cast<Direct3DTexture8 *>(pSourceTexture)->GetProxyInterface();
			DestinationBaseTextureInterface = static_cast<Direct3DTexture8 *>(pDestinationTexture)->GetProxyInterface();
			break;
		case D3DRTYPE_VOLUMETEXTURE:
			SourceBaseTextureInterface = static_cast<Direct3DVolumeTexture8 *>(pSourceTexture)->GetProxyInterface();
			DestinationBaseTextureInterface = static_cast<Direct3DVolumeTexture8 *>(pDestinationTexture)->GetProxyInterface();
			break;
		case D3DRTYPE_CUBETEXTURE:
			SourceBaseTextureInterface = static_cast<Direct3DCubeTexture8 *>(pSourceTexture)->GetProxyInterface();
			DestinationBaseTextureInterface = static_cast<Direct3DCubeTexture8 *>(pDestinationTexture)->GetProxyInterface();
			break;
		default:
			return D3DERR_INVALIDCALL;
	}

	return ProxyInterface->UpdateTexture(SourceBaseTextureInterface, DestinationBaseTextureInterface);
#endif
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::GetFrontBuffer(Direct3DSurface8* pDestSurface)
{
#if 1
	// not yet supported
	return D3DERR_INVALIDCALL;
#else
	if (pDestSurface == nullptr)
	{
		return D3DERR_INVALIDCALL;
	}

	return ProxyInterface->GetFrontBufferData(0, pDestSurface->GetProxyInterface());
#endif
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::SetRenderTarget(Direct3DSurface8* pRenderTarget, Direct3DSurface8* pNewZStencil)
{
	print_info_queue();

	ID3D11RenderTargetView* render_target_ = nullptr;
	ID3D11DepthStencilView* depth_stencil_ = nullptr;

	if (pRenderTarget != nullptr)
	{
		render_target_ = pRenderTarget->render_target.Get();

		if (!render_target_)
		{
			return D3DERR_INVALIDCALL;
		}

		current_render_target = pRenderTarget;

		D3DVIEWPORT8 viewport_ {};
		GetViewport(&viewport_);

		viewport_.Width  = pRenderTarget->desc8.Width;
		viewport_.Height = pRenderTarget->desc8.Height;

		SetViewport(&viewport_);
	}

	if (pNewZStencil != nullptr)
	{
		depth_stencil_ = pNewZStencil->depth_stencil.Get();

		if (!depth_stencil_)
		{
			return D3DERR_INVALIDCALL;
		}

		current_depth_stencil = pNewZStencil;
	}

	context->OMSetRenderTargets(1, &render_target_, depth_stencil_);
	print_info_queue();
	return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::GetRenderTarget(Direct3DSurface8** ppRenderTarget)
{
	if (!ppRenderTarget)
	{
		return D3DERR_INVALIDCALL;
	}

	*ppRenderTarget = current_render_target.Get();
	(*ppRenderTarget)->AddRef();
	return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::GetDepthStencilSurface(Direct3DSurface8** ppZStencilSurface)
{
	if (!ppZStencilSurface)
	{
		return D3DERR_INVALIDCALL;
	}

	*ppZStencilSurface = current_depth_stencil.Get();
	(*ppZStencilSurface)->AddRef();
	return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::BeginScene()
{
	return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::EndScene()
{
	return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::Clear(DWORD Count, const D3DRECT* pRects, DWORD Flags, D3DCOLOR Color, float Z, DWORD Stencil)
{
	if (Flags & D3DCLEAR_TARGET)
	{
		float color[] = {
			static_cast<float>((Color >> 16) & 0xFF) / 255.0f,
			static_cast<float>((Color >> 8) & 0xFF) / 255.0f,
			static_cast<float>(Color & 0xFF) / 255.0f,
			static_cast<float>((Color >> 24) & 0xFF) / 255.0f,
		};

		if (current_render_target)
		{
			context->ClearRenderTargetView(current_render_target->render_target.Get(), color);
		}
	}

	if (Flags & (D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL))
	{
		uint32_t flags = 0;

		if (Flags & D3DCLEAR_ZBUFFER)
		{
			flags |= D3D11_CLEAR_DEPTH;
		}

		if (Flags & D3DCLEAR_STENCIL)
		{
			flags |= D3D11_CLEAR_STENCIL;
		}

		if (current_depth_stencil)
		{
			context->ClearDepthStencilView(current_depth_stencil->depth_stencil.Get(), flags, Z, static_cast<uint8_t>(Stencil));
		}
	}

	return D3D_OK;
}

void Direct3DDevice8::update_wv_inv_t()
{
	const matrix m = (per_model.world_matrix.data() * per_scene.view_matrix.data()).Invert();

	// don't need to transpose for reasons
	per_model.wv_matrix_inv_t = m;
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::SetTransform(D3DTRANSFORMSTATETYPE State, const matrix* pMatrix)
{
	if (!pMatrix)
	{
		return D3DERR_INVALIDCALL;
	}

	switch (static_cast<uint32_t>(State))
	{
		case D3DTS_VIEW:
		{
			per_scene.view_matrix = *pMatrix;

			const auto inverse = per_scene.view_matrix.data().Invert();
			per_scene.view_position = inverse.Translation();

			update_wv_inv_t();
			break;
		}

		case D3DTS_PROJECTION:
			per_scene.projection_matrix = *pMatrix;
			break;

		case D3DTS_TEXTURE0:
		case D3DTS_TEXTURE1:
		case D3DTS_TEXTURE2:
		case D3DTS_TEXTURE3:
		case D3DTS_TEXTURE4:
		case D3DTS_TEXTURE5:
		case D3DTS_TEXTURE6:
		case D3DTS_TEXTURE7:
			per_texture.stages[State - D3DTS_TEXTURE0].transform = *pMatrix;
			break;

		case D3DTS_WORLD:
			per_model.world_matrix = *pMatrix;
			update_wv_inv_t();
			break;

		default:
			return D3DERR_INVALIDCALL;
	}

	return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::GetTransform(D3DTRANSFORMSTATETYPE State, matrix* pMatrix)
{
	if (!pMatrix)
	{
		return D3DERR_INVALIDCALL;
	}

	switch (static_cast<uint32_t>(State))
	{
		case D3DTS_VIEW:
			*pMatrix = per_scene.view_matrix;
			break;

		case D3DTS_PROJECTION:
			*pMatrix = per_scene.projection_matrix;
			break;

		case D3DTS_TEXTURE0:
		case D3DTS_TEXTURE1:
		case D3DTS_TEXTURE2:
		case D3DTS_TEXTURE3:
		case D3DTS_TEXTURE4:
		case D3DTS_TEXTURE5:
		case D3DTS_TEXTURE6:
		case D3DTS_TEXTURE7:
			*pMatrix = per_texture.stages[State - D3DTS_TEXTURE0].transform;
			break;

		case D3DTS_WORLD:
			*pMatrix = per_model.world_matrix;
			break;

		default:
			return D3DERR_INVALIDCALL;
	}

	return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::MultiplyTransform(D3DTRANSFORMSTATETYPE State, const matrix* pMatrix)
{
	if (!pMatrix)
	{
		return D3DERR_INVALIDCALL;
	}

	switch (static_cast<uint32_t>(State))
	{
		case D3DTS_VIEW:
			per_scene.view_matrix = per_scene.view_matrix * *pMatrix;
			break;

		case D3DTS_PROJECTION:
			per_scene.projection_matrix = per_scene.projection_matrix * *pMatrix;
			break;

		case D3DTS_TEXTURE0:
		case D3DTS_TEXTURE1:
		case D3DTS_TEXTURE2:
		case D3DTS_TEXTURE3:
		case D3DTS_TEXTURE4:
		case D3DTS_TEXTURE5:
		case D3DTS_TEXTURE6:
		case D3DTS_TEXTURE7:
		{
			auto& t = per_texture.stages[State - D3DTS_TEXTURE0].transform;
			t = t * *pMatrix;
			break;
		}

		case D3DTS_WORLD:
			per_model.world_matrix = per_model.world_matrix * *pMatrix;
			break;

		default:
			return D3DERR_INVALIDCALL;
	}

	return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::SetViewport(const D3DVIEWPORT8* pViewport)
{
#if 1
	if (!pViewport)
	{
		return D3DERR_INVALIDCALL;
	}

	viewport.Width    = static_cast<float>(pViewport->Width);
	viewport.Height   = static_cast<float>(pViewport->Height);
	viewport.MaxDepth = pViewport->MaxZ;
	viewport.MinDepth = pViewport->MinZ;
	viewport.TopLeftX = static_cast<float>(pViewport->X);
	viewport.TopLeftY = static_cast<float>(pViewport->Y);

	context->RSSetViewports(1, &viewport);
	return D3D_OK;
#else
	return ProxyInterface->SetViewport(pViewport);
#endif
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::GetViewport(D3DVIEWPORT8* pViewport)
{
	if (!pViewport)
	{
		return D3DERR_INVALIDCALL;
	}

	pViewport->Width  = static_cast<DWORD>(viewport.Width);
	pViewport->Height = static_cast<DWORD>(viewport.Height);
	pViewport->MaxZ   = viewport.MaxDepth;
	pViewport->MinZ   = viewport.MinDepth;
	pViewport->X      = static_cast<DWORD>(viewport.TopLeftX);
	pViewport->Y      = static_cast<DWORD>(viewport.TopLeftY);

	return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::SetMaterial(const D3DMATERIAL8* pMaterial)
{
	if (!pMaterial)
	{
		return D3DERR_INVALIDCALL;
	}

	material = *pMaterial;
	per_model.material = Material(material);
	return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::GetMaterial(D3DMATERIAL8* pMaterial)
{
	if (!pMaterial)
	{
		return D3DERR_INVALIDCALL;
	}

	*pMaterial = material;
	return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::SetLight(DWORD Index, const D3DLIGHT8* pLight)
{
	if (!pLight)
	{
		return D3DERR_INVALIDCALL;
	}

	if (Index >= per_model.lights.size())
	{
		return D3DERR_INVALIDCALL;
	}

	per_model.lights[Index] = Light(*pLight);
	return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::GetLight(DWORD Index, D3DLIGHT8* pLight)
{
	if (!pLight)
	{
		return D3DERR_INVALIDCALL;
	}

	if (Index >= per_model.lights.size())
	{
		return D3DERR_INVALIDCALL;
	}

	const auto& light = per_model.lights[Index].data();

	pLight->Type         = static_cast<D3DLIGHTTYPE>(light.type);
	pLight->Diffuse      = { light.diffuse.x, light.diffuse.y, light.diffuse.z, light.diffuse.w };
	pLight->Specular     = { light.diffuse.x, light.diffuse.y, light.diffuse.z, light.diffuse.w };
	pLight->Ambient      = { light.ambient.x, light.ambient.y, light.ambient.z, light.ambient.w };
	pLight->Position     = { light.position.x, light.position.y, light.position.z };
	pLight->Direction    = { light.direction.x, light.direction.y, light.direction.z };
	pLight->Range        = light.range;
	pLight->Falloff      = light.falloff;
	pLight->Attenuation0 = light.attenuation0;
	pLight->Attenuation1 = light.attenuation1;
	pLight->Attenuation2 = light.attenuation2;
	pLight->Theta        = light.theta;
	pLight->Phi          = light.phi;

	return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::LightEnable(DWORD Index, BOOL Enable)
{
	if (Index >= per_model.lights.size())
	{
		return D3DERR_INVALIDCALL;
	}

	Light light = per_model.lights[Index].data();
	light.enabled = Enable == TRUE;
	per_model.lights[Index] = light;
	return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::GetLightEnable(DWORD Index, BOOL* pEnable)
{
	if (!pEnable)
	{
		return D3DERR_INVALIDCALL;
	}

	if (Index >= per_model.lights.size())
	{
		return D3DERR_INVALIDCALL;
	}

	*pEnable = per_model.lights[Index].data().enabled;
	return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::SetClipPlane(DWORD Index, const float* pPlane)
{
	if (pPlane == nullptr || Index >= MAX_CLIP_PLANES)
	{
		return D3DERR_INVALIDCALL;
	}

	memcpy(stored_clip_planes[Index], pPlane, sizeof(stored_clip_planes[0]));
	return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::GetClipPlane(DWORD Index, float* pPlane)
{
	if (pPlane == nullptr || Index >= MAX_CLIP_PLANES)
	{
		return D3DERR_INVALIDCALL;
	}

	memcpy(pPlane, stored_clip_planes[Index], sizeof(stored_clip_planes[0]));
	return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::SetRenderState(D3DRENDERSTATETYPE State, DWORD Value)
{
	switch (static_cast<DWORD>(State))
	{
		case D3DRS_LINEPATTERN:
		case D3DRS_ZVISIBLE:
		case D3DRS_EDGEANTIALIAS:
		case D3DRS_PATCHSEGMENTS:
		case D3DRS_CLIPPLANEENABLE:
		case D3DRS_ZBIAS:
			return D3DERR_INVALIDCALL;

		case D3DRS_SOFTWAREVERTEXPROCESSING:
			return D3D_OK;

		default:
			break;
	}

	if (State < D3DRS_ZENABLE || State > D3DRS_NORMALORDER)
	{
		return D3DERR_INVALIDCALL;
	}

	// even if we do custom handling for a render state, we
	// store its value so the caller can retrieve it later in
	// Direct3DDevice8::GetRenderState
	auto& ref = render_state_values[State];
	ref = Value;

	auto set_stencil_flags = [&](auto shift)
	{
		auto flags = depthstencil_flags.stencil_flags.data();
		flags &= ~(StencilFlags::op_mask << shift);
		flags |= (Value & StencilFlags::op_mask) << shift;
		depthstencil_flags.stencil_flags = flags;
	};

	auto set_stencil_rw = [&](auto shift)
	{
		auto flags = depthstencil_flags.stencil_flags.data();
		flags &= ~(StencilFlags::rw_mask << shift);
		flags |= (Value & StencilFlags::rw_mask) << shift;
		depthstencil_flags.stencil_flags = flags;
	};

	switch (State)
	{
		default:
		{
			if (!ref.dirty())
			{
				break;
			}

			ref.clear();
			auto it = rs_strings.find(State);

			if (it == rs_strings.end())
			{
				break;
			}

			std::stringstream ss;
			ss << __FUNCTION__ << " unhandled render state type: "
				<< it->second << "; value: " << Value << std::endl;

			OutputDebugStringA(ss.str().c_str());
			break;
		}

		case D3DRS_COLORWRITEENABLE:
		{
			blend_flags = (blend_flags.data() & ~(0xF << BLEND_COLORMASK_SHIFT)) | ((Value & 0xF) << BLEND_COLORMASK_SHIFT);
			break;
		}

		case D3DRS_TEXTUREFACTOR:
			per_pixel.texture_factor = to_color4(Value);
			break;

		case D3DRS_FOGSTART:
			per_pixel.fog_start = *reinterpret_cast<float*>(&Value);
			break;

		case D3DRS_FOGEND:
			per_pixel.fog_end = *reinterpret_cast<float*>(&Value);
			break;

		case D3DRS_FOGCOLOR:
			per_pixel.fog_color = to_color4(Value);
			break;

		case D3DRS_FOGTABLEMODE:
			per_pixel.fog_mode = Value;
			break;

		case D3DRS_FOGDENSITY:
			per_pixel.fog_density = *reinterpret_cast<float*>(&Value);
			break;

		case D3DRS_SPECULARENABLE:
			if (Value != 0)
			{
				shader_flags |= ShaderFlags::rs_specular;
			}
			else
			{
				shader_flags &= ~ShaderFlags::rs_specular;
			}

			ref.clear();
			break;

		case D3DRS_LIGHTING:
			if (Value != 1)
			{
				shader_flags &= ~ShaderFlags::rs_lighting;
			}
			else
			{
				shader_flags |= ShaderFlags::rs_lighting;
			}

			ref.clear();
			break;

		case D3DRS_FOGENABLE:
			if (Value != 0)
			{
				shader_flags |= ShaderFlags::rs_fog;
			}
			else
			{
				shader_flags &= ~ShaderFlags::rs_fog;
			}

			ref.clear();
			break;

		case D3DRS_ALPHATESTENABLE:
		{
			per_pixel.alpha_reject = Value != 0;
			break;
		}

		case D3DRS_ALPHAFUNC:
		{
			per_pixel.alpha_reject_mode = Value;
			break;
		}

		case D3DRS_ALPHAREF:
		{
			per_pixel.alpha_reject_threshold = static_cast<float>(Value) / 255.0f;
			break;
		}

		case D3DRS_CULLMODE:
		{
			raster_flags = (raster_flags.data() & ~RasterFlags::cull_mask) | (Value & 3);
			break;
		}

		case D3DRS_FILLMODE:
		{
			raster_flags = (raster_flags.data() & ~RasterFlags::fill_mask) | ((Value & 3) << 2);
			break;
		}

		case D3DRS_ZENABLE:
		{
			if (Value)
			{
				depthstencil_flags.flags = depthstencil_flags.flags.data() | DepthStencilFlags::depth_test_enabled;
			}
			else
			{
				depthstencil_flags.flags = depthstencil_flags.flags.data() & ~DepthStencilFlags::depth_test_enabled;
			}

			ref.clear();
			break;
		}

		case D3DRS_ZFUNC:
		{
			if (!Value)
			{
				return D3DERR_INVALIDCALL;
			}

			depthstencil_flags.depth_flags = (depthstencil_flags.depth_flags.data() & ~DepthFlags::comparison_mask) | Value;
			ref.clear();
			break;
		}

		case D3DRS_ZWRITEENABLE:
			if (Value)
			{
				depthstencil_flags.flags = depthstencil_flags.flags.data() | DepthStencilFlags::depth_write_enabled;
			}
			else
			{
				depthstencil_flags.flags = depthstencil_flags.flags.data() & ~DepthStencilFlags::depth_write_enabled;
			}

			ref.clear();
			break;

		case D3DRS_STENCILENABLE:
			if (Value)
			{
				depthstencil_flags.flags = depthstencil_flags.flags.data() | DepthStencilFlags::stencil_enabled;
			}
			else
			{
				depthstencil_flags.flags = depthstencil_flags.flags.data() & ~DepthStencilFlags::stencil_enabled;
			}

			ref.clear();
			break;

		case D3DRS_STENCILFAIL:
			set_stencil_flags(StencilFlags::fail_shift);
			break;

		case D3DRS_STENCILZFAIL:
			set_stencil_flags(StencilFlags::zfail_shift);
			break;

		case D3DRS_STENCILPASS:
			set_stencil_flags(StencilFlags::pass_shift);
			break;

		case D3DRS_STENCILFUNC:
			set_stencil_flags(StencilFlags::func_shift);
			break;

		case D3DRS_STENCILREF:
			// value already stored; accessed in draw call
			break;

		case D3DRS_STENCILMASK:
			set_stencil_rw(StencilFlags::read_shift);
			break;

		case D3DRS_STENCILWRITEMASK:
			set_stencil_rw(StencilFlags::write_shift);
			break;

		case D3DRS_AMBIENT:
			per_model.ambient = to_color4(Value);
			break;

		case D3DRS_DIFFUSEMATERIALSOURCE:
			per_model.material_sources.diffuse = Value;
			break;

		case D3DRS_SPECULARMATERIALSOURCE:
			per_model.material_sources.specular = Value;
			break;

		case D3DRS_AMBIENTMATERIALSOURCE:
			per_model.material_sources.ambient = Value;
			break;

		case D3DRS_EMISSIVEMATERIALSOURCE:
			per_model.material_sources.emissive = Value;
			break;

		case D3DRS_COLORVERTEX:
			per_model.color_vertex = !!Value;
			break;

		case D3DRS_SRCBLEND:
			per_pixel.src_blend = Value;
			blend_flags = (blend_flags.data() & ~0x0F) | Value;
			break;

		case D3DRS_DESTBLEND:
			per_pixel.dst_blend = Value;
			blend_flags = (blend_flags.data() & ~0xF0) | (Value << 4);
			break;

		case D3DRS_ALPHABLENDENABLE:
			ref.clear();
			blend_flags = (blend_flags.data() & ~0x8000) | (Value ? 0x8000 : 0);

			if (Value != 1)
			{
				shader_flags &= ~ShaderFlags::rs_alpha;
			}
			else
			{
				shader_flags |= ShaderFlags::rs_alpha;
			}

			break;

		case D3DRS_BLENDOP:
			ref.clear();
			blend_flags = (blend_flags.data() & ~0xF00) | (Value << 8);
			per_pixel.blend_op = Value;
			break;
	}

	return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::GetRenderState(D3DRENDERSTATETYPE State, DWORD* pValue)
{
	if (pValue == nullptr)
	{
		return D3DERR_INVALIDCALL;
	}

	switch (static_cast<DWORD>(State))
	{
		case D3DRS_LINEPATTERN:
		case D3DRS_ZVISIBLE:
		case D3DRS_EDGEANTIALIAS:
		case D3DRS_PATCHSEGMENTS:
		case D3DRS_CLIPPLANEENABLE:
		case D3DRS_ZBIAS:
			return D3DERR_INVALIDCALL;

		case D3DRS_SOFTWAREVERTEXPROCESSING:
			*pValue = 0;
			return D3D_OK;

		default:
			break;
	}

	if (State < D3DRS_ZENABLE || State > D3DRS_NORMALORDER)
	{
		return D3DERR_INVALIDCALL;
	}

	*pValue = render_state_values[State];
	return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::BeginStateBlock()
{
#if 1
	// not yet supported
	return D3DERR_INVALIDCALL;
#else
	return ProxyInterface->BeginStateBlock();
#endif
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::EndStateBlock(DWORD* pToken)
{
#if 1
	// not yet supported
	return D3DERR_INVALIDCALL;
#else
	if (pToken == nullptr)
	{
		return D3DERR_INVALIDCALL;
	}

	return ProxyInterface->EndStateBlock(reinterpret_cast<IDirect3DStateBlock9 **>(pToken));
#endif
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::ApplyStateBlock(DWORD Token)
{
#if 1
	// not yet supported
	return D3DERR_INVALIDCALL;
#else
	if (Token == 0)
	{
		return D3DERR_INVALIDCALL;
	}

	return reinterpret_cast<IDirect3DStateBlock9 *>(Token)->Apply();
#endif
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::CaptureStateBlock(DWORD Token)
{
#if 1
	// not yet supported
	return D3DERR_INVALIDCALL;
#else
	if (Token == 0)
	{
		return D3DERR_INVALIDCALL;
	}

	return reinterpret_cast<IDirect3DStateBlock9 *>(Token)->Capture();
#endif
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::DeleteStateBlock(DWORD Token)
{
#if 1
	// not yet supported
	return D3DERR_INVALIDCALL;
#else
	if (Token == 0)
	{
		return D3DERR_INVALIDCALL;
	}

	reinterpret_cast<IDirect3DStateBlock9 *>(Token)->Release();

	return D3D_OK;
#endif
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::CreateStateBlock(D3DSTATEBLOCKTYPE Type, DWORD* pToken)
{
#if 1
	// not yet supported
	return D3DERR_INVALIDCALL;
#else
	if (pToken == nullptr)
	{
		return D3DERR_INVALIDCALL;
	}

	return ProxyInterface->CreateStateBlock(Type, reinterpret_cast<IDirect3DStateBlock9 **>(pToken));
#endif
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::SetClipStatus(const D3DCLIPSTATUS8* pClipStatus)
{
#if 1
	// not yet supported
	return D3DERR_INVALIDCALL;
#else
	return ProxyInterface->SetClipStatus(pClipStatus);
#endif
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::GetClipStatus(D3DCLIPSTATUS8* pClipStatus)
{
#if 1
	// not yet supported
	return D3DERR_INVALIDCALL;
#else
	return ProxyInterface->GetClipStatus(pClipStatus);
#endif
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::GetTexture(DWORD Stage, Direct3DBaseTexture8** ppTexture)
{
	if (ppTexture == nullptr)
	{
		return D3DERR_INVALIDCALL;
	}

	*ppTexture = nullptr;

	const auto it = textures.find(Stage);

	if (it == textures.end())
	{
		return D3DERR_INVALIDCALL;
	}

	auto texture = it->second;
	texture->AddRef();
	*ppTexture = it->second;
	return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::SetTexture(DWORD Stage, Direct3DBaseTexture8* pTexture)
{
	auto it = textures.find(Stage);

	if (pTexture == nullptr)
	{
		per_texture.stages[Stage].bound = false;

		ID3D11ShaderResourceView* bullshit[1] = {};
		context->PSSetShaderResources(Stage, 1, &bullshit[0]);

		if (it != textures.end())
		{
			safe_release(&it->second);
			textures.erase(it);
		}

		return D3D_OK;
	}

	switch (pTexture->GetType())
	{
		case D3DRTYPE_TEXTURE:
			break;
		default:
			return D3DERR_INVALIDCALL;
	}

	auto texture = dynamic_cast<Direct3DTexture8*>(pTexture);

	if (it != textures.end())
	{
		safe_release(&it->second);
		it->second = texture;
		texture->AddRef();
	}
	else
	{
		textures[Stage] = texture;
		texture->AddRef();
	}

	per_texture.stages[Stage].bound = true;
	context->PSSetShaderResources(Stage, 1, texture->srv.GetAddressOf());
	return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::GetTextureStageState(DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD* pValue)
{
	if (!pValue)
	{
		return D3DERR_INVALIDCALL;
	}

	switch (Type)
	{
		case D3DTSS_ADDRESSU:
			*pValue = sampler_setting_values[Stage].address_u.data();
			break;
		case D3DTSS_ADDRESSV:
			*pValue = sampler_setting_values[Stage].address_v.data();
			break;
		case D3DTSS_MAGFILTER:
			*pValue = sampler_setting_values[Stage].filter_mag.data();
			break;
		case D3DTSS_MINFILTER:
			*pValue = sampler_setting_values[Stage].filter_min.data();
			break;
		case D3DTSS_MIPFILTER:
			*pValue = sampler_setting_values[Stage].filter_mip.data();
			break;
		case D3DTSS_MIPMAPLODBIAS:
			*reinterpret_cast<float*>(pValue) = sampler_setting_values[Stage].mip_lod_bias.data();
			break;
		case D3DTSS_MAXMIPLEVEL:
			*pValue = sampler_setting_values[Stage].max_mip_level.data();
			break;
		case D3DTSS_MAXANISOTROPY:
			*pValue = sampler_setting_values[Stage].max_anisotropy.data();
			break;
		case D3DTSS_ADDRESSW:
			*pValue = sampler_setting_values[Stage].address_u.data();
			break;

		case D3DTSS_COLOROP:
			*pValue = per_texture.stages[Stage].color_op;
			break;
		case D3DTSS_COLORARG1:
			*pValue = per_texture.stages[Stage].color_arg1;
			break;
		case D3DTSS_COLORARG2:
			*pValue = per_texture.stages[Stage].color_arg2;
			break;
		case D3DTSS_ALPHAOP:
			*pValue = per_texture.stages[Stage].alpha_op;
			break;
		case D3DTSS_ALPHAARG1:
			*pValue = per_texture.stages[Stage].alpha_arg1;
			break;
		case D3DTSS_ALPHAARG2:
			*pValue = per_texture.stages[Stage].alpha_arg2;
			break;
		case D3DTSS_BUMPENVMAT00:
			*reinterpret_cast<float*>(pValue) = per_texture.stages[Stage].bump_env_mat00;
			break;
		case D3DTSS_BUMPENVMAT01:
			*reinterpret_cast<float*>(pValue) = per_texture.stages[Stage].bump_env_mat01;
			break;
		case D3DTSS_BUMPENVMAT10:
			*reinterpret_cast<float*>(pValue) = per_texture.stages[Stage].bump_env_mat10;
			break;
		case D3DTSS_BUMPENVMAT11:
			*reinterpret_cast<float*>(pValue) = per_texture.stages[Stage].bump_env_mat11;
			break;
		case D3DTSS_TEXCOORDINDEX:
			*pValue = per_texture.stages[Stage].tex_coord_index;
			break;
		// TODO: case D3DTSS_BORDERCOLOR:
		case D3DTSS_BUMPENVLSCALE:
			*reinterpret_cast<float*>(pValue) = per_texture.stages[Stage].bump_env_lscale;
			break;
		case D3DTSS_BUMPENVLOFFSET:
			*reinterpret_cast<float*>(pValue) = per_texture.stages[Stage].bump_env_loffset;
			break;
		case D3DTSS_TEXTURETRANSFORMFLAGS:
			*pValue = per_texture.stages[Stage].texture_transform_flags;
			break;
		case D3DTSS_COLORARG0:
			*pValue = per_texture.stages[Stage].color_arg0;
			break;
		case D3DTSS_ALPHAARG0:
			*pValue = per_texture.stages[Stage].alpha_arg0;
			break;
		// TODO: case D3DTSS_RESULTARG:

		default:
			return D3DERR_INVALIDCALL;
	}

	return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::SetTextureStageState(DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD Value)
{
	switch (Type)
	{
		case D3DTSS_ADDRESSU:
			sampler_setting_values[Stage].address_u = static_cast<D3DTEXTUREADDRESS>(Value);
			break;
		case D3DTSS_ADDRESSV:
			sampler_setting_values[Stage].address_v = static_cast<D3DTEXTUREADDRESS>(Value);
			break;
		case D3DTSS_MAGFILTER:
			sampler_setting_values[Stage].filter_mag = static_cast<D3DTEXTUREFILTERTYPE>(Value);
			break;
		case D3DTSS_MINFILTER:
			sampler_setting_values[Stage].filter_min = static_cast<D3DTEXTUREFILTERTYPE>(Value);
			break;
		case D3DTSS_MIPFILTER:
			sampler_setting_values[Stage].filter_mip = static_cast<D3DTEXTUREFILTERTYPE>(Value);
			break;
		case D3DTSS_MIPMAPLODBIAS:
			sampler_setting_values[Stage].mip_lod_bias = *reinterpret_cast<float*>(&Value);
			break;
		case D3DTSS_MAXMIPLEVEL:
			sampler_setting_values[Stage].max_mip_level = Value;
			break;
		case D3DTSS_MAXANISOTROPY:
			sampler_setting_values[Stage].max_anisotropy = Value;
			break;
		case D3DTSS_ADDRESSW:
			sampler_setting_values[Stage].address_w = static_cast<D3DTEXTUREADDRESS>(Value);
			break;

		case D3DTSS_COLOROP:
			per_texture.stages[Stage].color_op = static_cast<D3DTEXTUREOP>(Value);

			switch (Value)
			{
				case D3DTOP_PREMODULATE:
				case D3DTOP_BUMPENVMAP:
				case D3DTOP_BUMPENVMAPLUMINANCE:
				case D3DTOP_DOTPRODUCT3:
					OutputDebugStringA("WARNING: Unsupported texture blending operation!");
					break;

				default:
					break;
			}
			break;
		case D3DTSS_COLORARG1:
			per_texture.stages[Stage].color_arg1 = Value;
			break;
		case D3DTSS_COLORARG2:
			per_texture.stages[Stage].color_arg2 = Value;
			break;
		case D3DTSS_ALPHAOP:
			per_texture.stages[Stage].alpha_op = static_cast<D3DTEXTUREOP>(Value);

			switch (Value)
			{
				case D3DTOP_PREMODULATE:
				case D3DTOP_BUMPENVMAP:
				case D3DTOP_BUMPENVMAPLUMINANCE:
				case D3DTOP_DOTPRODUCT3:
					OutputDebugStringA("WARNING: Unsupported texture blending operation!");
					break;

				default:
					break;
			}
			break;
		case D3DTSS_ALPHAARG1:
			per_texture.stages[Stage].alpha_arg1 = Value;
			break;
		case D3DTSS_ALPHAARG2:
			per_texture.stages[Stage].alpha_arg2 = Value;
			break;
		case D3DTSS_BUMPENVMAT00:
			per_texture.stages[Stage].bump_env_mat00 = *reinterpret_cast<float*>(&Value);
			break;
		case D3DTSS_BUMPENVMAT01:
			per_texture.stages[Stage].bump_env_mat01 = *reinterpret_cast<float*>(&Value);
			break;
		case D3DTSS_BUMPENVMAT10:
			per_texture.stages[Stage].bump_env_mat10 = *reinterpret_cast<float*>(&Value);
			break;
		case D3DTSS_BUMPENVMAT11:
			per_texture.stages[Stage].bump_env_mat11 = *reinterpret_cast<float*>(&Value);
			break;
		case D3DTSS_TEXCOORDINDEX:
			per_texture.stages[Stage].tex_coord_index = Value;
			break;
		case D3DTSS_BUMPENVLSCALE:
			per_texture.stages[Stage].bump_env_lscale = *reinterpret_cast<float*>(&Value);
			break;
		case D3DTSS_BUMPENVLOFFSET:
			per_texture.stages[Stage].bump_env_loffset = *reinterpret_cast<float*>(&Value);
			break;
		case D3DTSS_TEXTURETRANSFORMFLAGS:
			per_texture.stages[Stage].texture_transform_flags = static_cast<D3DTEXTURETRANSFORMFLAGS>(Value);
			break;
		case D3DTSS_COLORARG0:
			per_texture.stages[Stage].color_arg0 = Value;
			break;
		case D3DTSS_ALPHAARG0:
			per_texture.stages[Stage].alpha_arg0 = Value;
			break;
		//case D3DTSS_RESULTARG: // TODO

		default:
			return D3DERR_INVALIDCALL;
	}

	return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::ValidateDevice(DWORD* pNumPasses)
{
#if 1
	// not yet supported
	return D3DERR_INVALIDCALL;
#else
	return ProxyInterface->ValidateDevice(pNumPasses);
#endif
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::GetInfo(DWORD DevInfoID, void* pDevInfoStruct, DWORD DevInfoStructSize)
{
	UNREFERENCED_PARAMETER(DevInfoID);
	UNREFERENCED_PARAMETER(pDevInfoStruct);
	UNREFERENCED_PARAMETER(DevInfoStructSize);

	return S_FALSE;
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::SetPaletteEntries(UINT PaletteNumber, const PALETTEENTRY* pEntries)
{
#if 1
	// not yet supported
	return D3DERR_INVALIDCALL;
#else
	if (pEntries == nullptr)
	{
		return D3DERR_INVALIDCALL;
	}
	return ProxyInterface->SetPaletteEntries(PaletteNumber, pEntries);
#endif
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::GetPaletteEntries(UINT PaletteNumber, PALETTEENTRY* pEntries)
{
#if 1
	// not yet supported
	return D3DERR_INVALIDCALL;
#else
	if (pEntries == nullptr)
	{
		return D3DERR_INVALIDCALL;
	}
	return ProxyInterface->GetPaletteEntries(PaletteNumber, pEntries);
#endif
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::SetCurrentTexturePalette(UINT PaletteNumber)
{
#if 1
	// not yet supported
	return D3DERR_INVALIDCALL;
#else
	if (!PaletteFlag)
	{
		return D3DERR_INVALIDCALL;
	}
	return ProxyInterface->SetCurrentTexturePalette(PaletteNumber);
#endif
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::GetCurrentTexturePalette(UINT* pPaletteNumber)
{
#if 1
	// not yet supported
	return D3DERR_INVALIDCALL;
#else
	if (!PaletteFlag)
	{
		return D3DERR_INVALIDCALL;
	}
	return ProxyInterface->GetCurrentTexturePalette(pPaletteNumber);
#endif
}

void Direct3DDevice8::run_draw_prologues(const std::string& callback)
{
	const auto& preproc = shader_preprocess(shader_flags);
	for (auto& fn : draw_prologues[callback])
	{
		fn(preproc, shader_flags);
	}
}

void Direct3DDevice8::run_draw_epilogues(const std::string& callback)
{
	const auto& preproc = shader_preprocess(shader_flags);
	for (auto& fn : draw_epilogues[callback])
	{
		fn(preproc, shader_flags);
	}
}

bool Direct3DDevice8::set_primitive_type(D3DPRIMITIVETYPE primitive_type) const
{
	const auto topology = to_d3d11(primitive_type);

	if (topology != D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED)
	{
		context->IASetPrimitiveTopology(topology);
		return true;
	}

	return false;
}

bool Direct3DDevice8::primitive_vertex_count(D3DPRIMITIVETYPE primitive_type, uint32_t& count)
{
	switch (primitive_type)
	{
		case D3DPT_TRIANGLELIST:
			count *= 3;
			break;

		case D3DPT_TRIANGLESTRIP:
			count += 2;
			break;

		case D3DPT_POINTLIST:
			break;

		case D3DPT_LINELIST:
			count *= 2;
			break;

		case D3DPT_LINESTRIP:
			++count;
			break;

		case D3DPT_TRIANGLEFAN:
			//printf(__FUNCTION__ ": D3DPT_TRIANGLEFAN not implemented\n");
			return false;

		default:
			return false;
	}

	return true;
}

void Direct3DDevice8::oit_zwrite_force(DWORD& ZWRITEENABLE, DWORD& ZENABLE)
{
	GetRenderState(D3DRS_ZWRITEENABLE, &ZWRITEENABLE);
	GetRenderState(D3DRS_ZENABLE, &ZENABLE);

	if (shader_flags & ShaderFlags::rs_alpha && (oit_actually_enabled && oit_enabled))
	{
		// force zwrite on to enable writing 100% opaque
		// pixels to the real backbuffer and depth buffer.
		SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
		SetRenderState(D3DRS_ZENABLE, TRUE); // this does nothing right now, but good practice!

		update_depth();
	}
}

void Direct3DDevice8::oit_zwrite_restore(DWORD ZWRITEENABLE, DWORD ZENABLE)
{
	SetRenderState(D3DRS_ZWRITEENABLE, ZWRITEENABLE);
	SetRenderState(D3DRS_ZENABLE, ZENABLE);

	update_depth();
}

// the other draw function (UP) gets routed through here
HRESULT STDMETHODCALLTYPE Direct3DDevice8::DrawPrimitive(D3DPRIMITIVETYPE PrimitiveType, UINT StartVertex, UINT PrimitiveCount)
{
	if (PrimitiveType == D3DPT_TRIANGLEFAN)
	{
		auto stream = stream_sources[0]; // HACK: shouldn't only be handling 0!

		if (!stream.buffer)
		{
			return D3DERR_INVALIDCALL;
		}

		const auto buffer = stream.buffer;
		const auto stride = stream.stride;
		const auto offset = StartVertex * stride;
		const auto size   = (2 + PrimitiveCount) * stride;

		uint8_t* data = nullptr;
		buffer->get_buffer(offset, size, &data);

		ComPtr<Direct3DVertexBuffer8> temp;
		UINT temp_stride;
		GetStreamSource(0, &temp, &temp_stride);
		const auto result = DrawPrimitiveUP(PrimitiveType, PrimitiveCount, data, stride);
		SetStreamSource(0, temp.Get(), temp_stride);
		return result;
	}

	if (!set_primitive_type(PrimitiveType))
	{
		return D3DERR_INVALIDCALL;
	}

	draw_call_increment();
	if (!update())
	{
		return D3DERR_INVALIDCALL;
	}

	if (skip_draw())
	{
		//OutputDebugStringA("WARNING: SKIPPING DRAW CALL\n");
		return D3D_OK;
	}

	uint32_t count = PrimitiveCount;

	if (!primitive_vertex_count(PrimitiveType, count))
	{
		return D3DERR_INVALIDCALL;
	}

	DWORD ZWRITEENABLE, ZENABLE;
	oit_zwrite_force(ZWRITEENABLE, ZENABLE);

	run_draw_prologues(__FUNCTION__);
	context->Draw(count, StartVertex);
	run_draw_epilogues(__FUNCTION__);

	oit_zwrite_restore(ZWRITEENABLE, ZENABLE);
	return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::DrawIndexedPrimitive(D3DPRIMITIVETYPE PrimitiveType, UINT MinIndex, UINT NumVertices, UINT StartIndex, UINT PrimitiveCount)
{
	if (!index_buffer)
	{
		return D3DERR_INVALIDCALL;
	}

	// TODO: triangle fan
	if (PrimitiveType == D3DPT_TRIANGLEFAN)
	{
		return D3DERR_INVALIDCALL;
	}

	if (!set_primitive_type(PrimitiveType))
	{
		return D3DERR_INVALIDCALL;
	}

	draw_call_increment();
	if (!update())
	{
		return D3DERR_INVALIDCALL;
	}

	if (skip_draw())
	{
		//OutputDebugStringA("WARNING: SKIPPING DRAW CALL\n");
		return D3D_OK;
	}

	auto count = PrimitiveCount;
	primitive_vertex_count(PrimitiveType, count);

	DWORD ZWRITEENABLE, ZENABLE;
	oit_zwrite_force(ZWRITEENABLE, ZENABLE);

	run_draw_prologues(__FUNCTION__);
	context->DrawIndexed(count, StartIndex, current_base_vertex_index);
	run_draw_epilogues(__FUNCTION__);

	oit_zwrite_restore(ZWRITEENABLE, ZENABLE);
	return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::DrawPrimitiveUP(D3DPRIMITIVETYPE PrimitiveType, UINT PrimitiveCount, const void* pVertexStreamZeroData, UINT VertexStreamZeroStride)
{
	if (!pVertexStreamZeroData)
	{
		return D3DERR_INVALIDCALL;
	}

	if (PrimitiveType == D3DPT_TRIANGLEFAN)
	{
		const auto data   = reinterpret_cast<const uint8_t*>(pVertexStreamZeroData);
		const auto stride = VertexStreamZeroStride;

		trifan_buffer.resize(3 * stride * PrimitiveCount);

		auto buffer = trifan_buffer.data();

		const auto v0 = &data[0];
		auto vx = &data[stride];

		for (size_t i = 0; i < PrimitiveCount; i++)
		{
			// 0
			memcpy(buffer, v0, stride);
			buffer += stride;

			// last (or second from 0) vertex
			memcpy(buffer, vx, stride);
			buffer += stride;

			// next vertex
			vx += stride;
			memcpy(buffer, vx, stride);
			buffer += stride;
		}

		return DrawPrimitiveUP(D3DPT_TRIANGLELIST, PrimitiveCount, trifan_buffer.data(), stride);
	}

	if (!set_primitive_type(PrimitiveType))
	{
		return D3DERR_INVALIDCALL;
	}

	uint32_t count = PrimitiveCount;

	if (!primitive_vertex_count(PrimitiveType, count))
	{
		return D3DERR_INVALIDCALL;
	}

	draw_call_increment();
	if (!update())
	{
		return D3DERR_INVALIDCALL;
	}

	if (skip_draw())
	{
		//OutputDebugStringA("WARNING: SKIPPING DRAW CALL\n");
		return D3D_OK;
	}

	const auto size = count * VertexStreamZeroStride;

	up_get(size);

	if (up_buffer == nullptr)
	{
		return D3DERR_INVALIDCALL;
	}

	BYTE* ptr;
	up_buffer->Lock(0, size, &ptr, D3DLOCK_DISCARD);

	memcpy(ptr, pVertexStreamZeroData, size);

	up_buffer->Unlock();

	SetStreamSource(0, up_buffer.Get(), VertexStreamZeroStride);

	run_draw_prologues(__FUNCTION__);
	const auto result = DrawPrimitive(PrimitiveType, 0, PrimitiveCount);
	run_draw_epilogues(__FUNCTION__);

	SetStreamSource(0, nullptr, 0);
	up_buffers.push_back(up_buffer);
	up_buffer = nullptr;

	return result;
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::DrawIndexedPrimitiveUP(D3DPRIMITIVETYPE PrimitiveType, UINT MinVertexIndex, UINT NumVertexIndices, UINT PrimitiveCount, const void* pIndexData, D3DFORMAT IndexDataFormat, const void* pVertexStreamZeroData, UINT VertexStreamZeroStride)
{
#if 1
	// not yet supported
	return D3DERR_INVALIDCALL;
#else
	ApplyClipPlanes();
	return ProxyInterface->DrawIndexedPrimitiveUP(PrimitiveType, MinVertexIndex, NumVertexIndices, PrimitiveCount, pIndexData, IndexDataFormat, pVertexStreamZeroData, VertexStreamZeroStride);
#endif
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::ProcessVertices(UINT SrcStartIndex, UINT DestIndex, UINT VertexCount, Direct3DVertexBuffer8* pDestBuffer, DWORD Flags)
{
#if 1
	// not yet supported
	return D3DERR_INVALIDCALL;
#else
	if (pDestBuffer == nullptr)
	{
		return D3DERR_INVALIDCALL;
	}

	return ProxyInterface->ProcessVertices(SrcStartIndex, DestIndex, VertexCount, pDestBuffer->GetProxyInterface(), nullptr, Flags);
#endif
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::CreateVertexShader(const DWORD* pDeclaration, const DWORD* pFunction, DWORD* pHandle, DWORD Usage)
{
	// not yet supported
	return D3DERR_INVALIDCALL;
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::SetVertexShader(DWORD Handle)
{
	HRESULT hr;

	if ((Handle & 0x80000000) == 0)
	{
		if ((Handle & D3DFVF_XYZRHW) && D3DFVF_XYZRHW != (Handle & (D3DFVF_XYZRHW | D3DFVF_XYZ | D3DFVF_NORMAL)))
		{
			return D3DERR_INVALIDCALL;
		}

		const auto fvf = fvf_sanitize(Handle);

		shader_flags &= ~ShaderFlags::fvf_mask;
		shader_flags |= fvf;
		FVF = fvf;

		current_vertex_shader_handle = 0;
		hr = D3D_OK;
	}
	else
	{
	#if 1
		// not yet supported
		hr = D3DERR_INVALIDCALL;
	#else
		const DWORD handleMagic = Handle << 1;
		VertexShaderInfo *const ShaderInfo = reinterpret_cast<VertexShaderInfo *>(handleMagic);

		hr = ProxyInterface->SetVertexShader(ShaderInfo->Shader);
		ProxyInterface->SetVertexDeclaration(ShaderInfo->Declaration);

		CurrentVertexShaderHandle = Handle;
	#endif
	}

	return hr;
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::GetVertexShader(DWORD* pHandle)
{
	if (pHandle == nullptr)
	{
		return D3DERR_INVALIDCALL;
	}

	if (current_vertex_shader_handle == 0)
	{
		*pHandle = FVF;
		return D3D_OK;
	}
#if 1
	// not yet supported
	return D3DERR_INVALIDCALL;
#else
	*pHandle = CurrentVertexShaderHandle;

	return D3D_OK;
#endif
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::DeleteVertexShader(DWORD Handle)
{
#if 1
	// not yet supported
	return D3DERR_INVALIDCALL;
#else
	if ((Handle & 0x80000000) == 0)
	{
		return D3DERR_INVALIDCALL;
	}

	if (CurrentVertexShaderHandle == Handle)
	{
		SetVertexShader(0);
	}

	const DWORD HandleMagic = Handle << 1;
	VertexShaderInfo *const ShaderInfo = reinterpret_cast<VertexShaderInfo *>(HandleMagic);

	if (ShaderInfo->Shader != nullptr)
	{
		ShaderInfo->Shader->Release();
	}
	if (ShaderInfo->Declaration != nullptr)
	{
		ShaderInfo->Declaration->Release();
	}

	delete ShaderInfo;

	return D3D_OK;
#endif
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::SetVertexShaderConstant(DWORD Register, const void* pConstantData, DWORD ConstantCount)
{
#if 1
	// not yet supported
	return D3DERR_INVALIDCALL;
#else
	return ProxyInterface->SetVertexShaderConstantF(Register, static_cast<const float *>(pConstantData), ConstantCount);
#endif
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::GetVertexShaderConstant(DWORD Register, void* pConstantData, DWORD ConstantCount)
{
#if 1
	// not yet supported
	return D3DERR_INVALIDCALL;
#else
	return ProxyInterface->GetVertexShaderConstantF(Register, static_cast<float *>(pConstantData), ConstantCount);
#endif
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::GetVertexShaderDeclaration(DWORD Handle, void* pData, DWORD* pSizeOfData)
{
	UNREFERENCED_PARAMETER(Handle);
	UNREFERENCED_PARAMETER(pData);
	UNREFERENCED_PARAMETER(pSizeOfData);

	return D3DERR_INVALIDCALL;
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::GetVertexShaderFunction(DWORD Handle, void* pData, DWORD* pSizeOfData)
{
#if 1
	// not yet supported
	return D3DERR_INVALIDCALL;
#else
	if ((Handle & 0x80000000) == 0)
	{
		return D3DERR_INVALIDCALL;
	}

	const DWORD HandleMagic = Handle << 1;
	IDirect3DVertexShader9 *VertexShaderInterface = reinterpret_cast<VertexShaderInfo *>(HandleMagic)->Shader;

	if (VertexShaderInterface == nullptr)
	{
		return D3DERR_INVALIDCALL;
	}

	return VertexShaderInterface->GetFunction(pData, reinterpret_cast<UINT *>(pSizeOfData));
#endif
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::SetStreamSource(UINT StreamNumber, Direct3DVertexBuffer8* pStreamData, UINT Stride)
{
	const StreamPair pair = { pStreamData, pStreamData ? Stride : 0 };
	auto it = stream_sources.find(StreamNumber);

	if (it == stream_sources.end())
	{
		stream_sources[StreamNumber] = pair;
		safe_addref(pStreamData);
	}
	else
	{
		if (it->second == pair)
		{
			return D3D_OK;
		}

		safe_release(&it->second.buffer);

		it->second = pair;

		safe_addref(pStreamData);
	}

	if (pStreamData == nullptr)
	{
		context->IASetVertexBuffers(StreamNumber, 0, nullptr, nullptr, nullptr);
	}
	else
	{
		UINT zero = 0;
		context->IASetVertexBuffers(StreamNumber, 1, pStreamData->buffer_resource.GetAddressOf(), &Stride, &zero);
	}

	return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::GetStreamSource(UINT StreamNumber, Direct3DVertexBuffer8** ppStreamData, UINT* pStride)
{
	if (ppStreamData == nullptr)
	{
		return D3DERR_INVALIDCALL;
	}

	*ppStreamData = nullptr;

	if (pStride)
	{
		*pStride = 0;
	}

	auto it = stream_sources.find(StreamNumber);

	if (it != stream_sources.end() && it->second.buffer)
	{
		*ppStreamData = it->second.buffer;
		it->second.buffer->AddRef();

		if (pStride)
		{
			*pStride = it->second.stride;
		}
	}

	return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::SetIndices(Direct3DIndexBuffer8* pIndexData, UINT BaseVertexIndex)
{
	if (BaseVertexIndex > 0x7FFFFFFF)
	{
		return D3DERR_INVALIDCALL;
	}

	if (pIndexData == nullptr)
	{
		if (pIndexData != index_buffer.Get())
		{
			index_buffer = pIndexData;
			context->IASetIndexBuffer(nullptr, DXGI_FORMAT_UNKNOWN, 0);
		}

		return D3D_OK;
	}

	index_buffer = pIndexData;
	current_base_vertex_index = static_cast<INT>(BaseVertexIndex);
	const auto dxgi = to_dxgi(index_buffer->desc8.Format);
	context->IASetIndexBuffer(index_buffer->buffer_resource.Get(), dxgi, 0);
	return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::GetIndices(Direct3DIndexBuffer8** ppIndexData, UINT* pBaseVertexIndex)
{
	if (ppIndexData == nullptr)
	{
		return D3DERR_INVALIDCALL;
	}

	if (index_buffer)
	{
		*ppIndexData = index_buffer.Get();
		(*ppIndexData)->AddRef();
	}
	else
	{
		*ppIndexData = nullptr;
	}

	if (pBaseVertexIndex != nullptr)
	{
		*pBaseVertexIndex = static_cast<UINT>(current_base_vertex_index);
	}

	return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::CreatePixelShader(const DWORD* pFunction, DWORD* pHandle)
{
	return D3DERR_INVALIDCALL;
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::SetPixelShader(DWORD Handle)
{
#if 1
	// not yet supported
	return D3DERR_INVALIDCALL;
#else
	CurrentPixelShaderHandle = Handle;

	return ProxyInterface->SetPixelShader(reinterpret_cast<IDirect3DPixelShader9 *>(Handle));
#endif
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::GetPixelShader(DWORD* pHandle)
{
#if 1
	// not yet supported
	return D3DERR_INVALIDCALL;
#else
	if (pHandle == nullptr)
	{
		return D3DERR_INVALIDCALL;
	}

	*pHandle = CurrentPixelShaderHandle;

	return D3D_OK;
#endif
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::DeletePixelShader(DWORD Handle)
{
#if 1
	// not yet supported
	return D3DERR_INVALIDCALL;
#else
	if (Handle == 0)
	{
		return D3DERR_INVALIDCALL;
	}

	if (CurrentPixelShaderHandle == Handle)
	{
		SetPixelShader(0);
	}

	reinterpret_cast<IDirect3DPixelShader9 *>(Handle)->Release();

	return D3D_OK;
#endif
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::SetPixelShaderConstant(DWORD Register, const void* pConstantData, DWORD ConstantCount)
{
#if 1
	// not yet supported
	return D3DERR_INVALIDCALL;
#else
	return ProxyInterface->SetPixelShaderConstantF(Register, static_cast<const float *>(pConstantData), ConstantCount);
#endif
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::GetPixelShaderConstant(DWORD Register, void* pConstantData, DWORD ConstantCount)
{
#if 1
	// not yet supported
	return D3DERR_INVALIDCALL;
#else
	return ProxyInterface->GetPixelShaderConstantF(Register, static_cast<float *>(pConstantData), ConstantCount);
#endif
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::GetPixelShaderFunction(DWORD Handle, void* pData, DWORD* pSizeOfData)
{
	return D3DERR_INVALIDCALL;
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::DrawRectPatch(UINT Handle, const float* pNumSegs, const D3DRECTPATCH_INFO* pRectPatchInfo)
{
#if 1
	// not yet supported
	return D3DERR_INVALIDCALL;
#else
	return ProxyInterface->DrawRectPatch(Handle, pNumSegs, pRectPatchInfo);
#endif
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::DrawTriPatch(UINT Handle, const float* pNumSegs, const D3DTRIPATCH_INFO* pTriPatchInfo)
{
#if 1
	// not yet supported
	return D3DERR_INVALIDCALL;
#else
	return ProxyInterface->DrawTriPatch(Handle, pNumSegs, pTriPatchInfo);
#endif
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::DeletePatch(UINT Handle)
{
#if 1
	// not yet supported
	return D3DERR_INVALIDCALL;
#else
	return ProxyInterface->DeletePatch(Handle);
#endif
}

void Direct3DDevice8::print_info_queue() const
{
#ifndef DEBUG
	if (!info_queue)
	{
		return;
	}

	UINT64 i = 0;

	do
	{
		SIZE_T size = 0;
		HRESULT hr = info_queue->GetMessageW(i, nullptr, &size);

		if (hr != S_FALSE)
		{
			break;
		}

		if (!size)
		{
			break;
		}

		auto pMessage = reinterpret_cast<D3D11_MESSAGE*>(new uint8_t[size]);

		hr = info_queue->GetMessageW(i, pMessage, &size);

		if (hr == S_OK && pMessage->pDescription)
		{
			OutputDebugStringA(pMessage->pDescription);
			//printf("%s\n", pMessage->pDescription);
		}

		delete[] pMessage;

		++i;
	} while (true);

	info_queue->ClearStoredMessages();
#endif
}

bool Direct3DDevice8::update_input_layout()
{
	auto key = fvf_sanitize(shader_flags & ShaderFlags::fvf_mask);
	DWORD fvf = key;
	FVF.clear();

	auto it = fvf_layouts.find(key);

	if (it != fvf_layouts.end())
	{
		context->IASetInputLayout(it->second.Get());
		return true;
	}

	if (!FVF.data())
	{
		return true;
	}

	D3D11_INPUT_ELEMENT_DESC pos_element {};
	pos_element.InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA;

	switch (fvf & D3DFVF_POSITION_MASK)
	{
		case D3DFVF_XYZ:
			pos_element.SemanticName = "POSITION";
			pos_element.Format       = DXGI_FORMAT_R32G32B32_FLOAT;
			break;

		case D3DFVF_XYZRHW:
			pos_element.SemanticName = "POSITION";
			pos_element.Format       = DXGI_FORMAT_R32G32B32A32_FLOAT;
			break;

		default:
			throw std::runtime_error("unsupported D3DFVF_POSITION type");
	}

	D3D11_INPUT_ELEMENT_DESC elements[16] {};

	size_t i = 0;
	fvf &= ~D3DFVF_POSITION_MASK;
	elements[i++] = pos_element;

	if (fvf & D3DFVF_NORMAL)
	{
		fvf ^= D3DFVF_NORMAL;
		D3D11_INPUT_ELEMENT_DESC e {};

		e.SemanticName      = "NORMAL";
		e.InputSlotClass    = D3D11_INPUT_PER_VERTEX_DATA;
		e.Format            = DXGI_FORMAT_R32G32B32_FLOAT;
		e.AlignedByteOffset = D3D11_APPEND_ALIGNED_ELEMENT;

		elements[i++] = e;
	}

	if (fvf & D3DFVF_DIFFUSE)
	{
		fvf ^= D3DFVF_DIFFUSE;
		D3D11_INPUT_ELEMENT_DESC e {};

		e.SemanticName      = "COLOR";
		e.SemanticIndex     = 0;
		e.InputSlotClass    = D3D11_INPUT_PER_VERTEX_DATA;
		e.Format            = DXGI_FORMAT_B8G8R8A8_UNORM;
		e.AlignedByteOffset = D3D11_APPEND_ALIGNED_ELEMENT;

		elements[i++] = e;
	}

	if (fvf & D3DFVF_SPECULAR)
	{
		fvf ^= D3DFVF_SPECULAR;
		D3D11_INPUT_ELEMENT_DESC e {};

		e.SemanticName      = "COLOR";
		e.SemanticIndex     = 1;
		e.InputSlotClass    = D3D11_INPUT_PER_VERTEX_DATA;
		e.Format            = DXGI_FORMAT_B8G8R8A8_UNORM;
		e.AlignedByteOffset = D3D11_APPEND_ALIGNED_ELEMENT;

		elements[i++] = e;
	}

	auto tex = (fvf & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT;

	if (tex > 0)
	{
		fvf &= ~D3DFVF_TEXCOUNT_MASK;

		if (tex >= 1)
		{
			D3D11_INPUT_ELEMENT_DESC e {};

			e.SemanticName      = "TEXCOORD";
			e.SemanticIndex     = 0;
			e.InputSlotClass    = D3D11_INPUT_PER_VERTEX_DATA;
			e.Format            = DXGI_FORMAT_R32G32_FLOAT;
			e.AlignedByteOffset = D3D11_APPEND_ALIGNED_ELEMENT;

			elements[i++] = e;
		}

		if (tex >= 2)
		{
			D3D11_INPUT_ELEMENT_DESC e {};

			e.SemanticName      = "TEXCOORD";
			e.SemanticIndex     = 1;
			e.InputSlotClass    = D3D11_INPUT_PER_VERTEX_DATA;
			e.Format            = DXGI_FORMAT_R32G32_FLOAT;
			e.AlignedByteOffset = D3D11_APPEND_ALIGNED_ELEMENT;

			elements[i++] = e;
		}

		if (tex >= 3)
		{
			D3D11_INPUT_ELEMENT_DESC e {};

			e.SemanticName      = "TEXCOORD";
			e.SemanticIndex     = 2;
			e.InputSlotClass    = D3D11_INPUT_PER_VERTEX_DATA;
			e.Format            = DXGI_FORMAT_R32G32_FLOAT;
			e.AlignedByteOffset = D3D11_APPEND_ALIGNED_ELEMENT;

			elements[i++] = e;
		}

		if (tex >= 4)
		{
			D3D11_INPUT_ELEMENT_DESC e {};

			e.SemanticName      = "TEXCOORD";
			e.SemanticIndex     = 3;
			e.InputSlotClass    = D3D11_INPUT_PER_VERTEX_DATA;
			e.Format            = DXGI_FORMAT_R32G32_FLOAT;
			e.AlignedByteOffset = D3D11_APPEND_ALIGNED_ELEMENT;

			elements[i++] = e;
		}

		if (tex >= 5)
		{
			D3D11_INPUT_ELEMENT_DESC e {};

			e.SemanticName      = "TEXCOORD";
			e.SemanticIndex     = 4;
			e.InputSlotClass    = D3D11_INPUT_PER_VERTEX_DATA;
			e.Format            = DXGI_FORMAT_R32G32_FLOAT;
			e.AlignedByteOffset = D3D11_APPEND_ALIGNED_ELEMENT;

			elements[i++] = e;
		}

		if (tex >= 6)
		{
			D3D11_INPUT_ELEMENT_DESC e {};

			e.SemanticName      = "TEXCOORD";
			e.SemanticIndex     = 5;
			e.InputSlotClass    = D3D11_INPUT_PER_VERTEX_DATA;
			e.Format            = DXGI_FORMAT_R32G32_FLOAT;
			e.AlignedByteOffset = D3D11_APPEND_ALIGNED_ELEMENT;

			elements[i++] = e;
		}

		if (tex >= 7)
		{
			D3D11_INPUT_ELEMENT_DESC e {};

			e.SemanticName      = "TEXCOORD";
			e.SemanticIndex     = 6;
			e.InputSlotClass    = D3D11_INPUT_PER_VERTEX_DATA;
			e.Format            = DXGI_FORMAT_R32G32_FLOAT;
			e.AlignedByteOffset = D3D11_APPEND_ALIGNED_ELEMENT;

			elements[i++] = e;
		}

		if (tex == 8)
		{
			D3D11_INPUT_ELEMENT_DESC e {};

			e.SemanticName      = "TEXCOORD";
			e.SemanticIndex     = 7;
			e.InputSlotClass    = D3D11_INPUT_PER_VERTEX_DATA;
			e.Format            = DXGI_FORMAT_R32G32_FLOAT;
			e.AlignedByteOffset = D3D11_APPEND_ALIGNED_ELEMENT;

			elements[i++] = e;
		}
	}

	if (i >= 16)
	{
		throw;
	}

	if (fvf != 0)
	{
		//printf("unsupported FVF\n");
		return false;
	}

	auto vs = get_vs(shader_flags, true, vertex_speed_shaders, vs_speed_mutex);

	ComPtr<ID3D11InputLayout> layout;

	HRESULT hr = device->CreateInputLayout(elements, i,
	                                       vs.blob->GetBufferPointer(), vs.blob->GetBufferSize(), &layout);

	if (FAILED(hr))
	{
		//throw std::runtime_error("CreateInputLayout failed");
		//printf("CreateInputLayout failed\n");
		return false;
	}

	fvf_layouts[key] = layout;
	context->IASetInputLayout(layout.Get());

	std::stringstream ss;
	ss << "Created input layout #" << fvf_layouts.size() << std::endl;
	OutputDebugStringA(ss.str().c_str());

	return true;
}

void Direct3DDevice8::commit_per_pixel()
{
	if (!per_pixel.dirty())
	{
		return;
	}

	D3D11_MAPPED_SUBRESOURCE mapped {};
	context->Map(per_pixel_cbuf.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);

	auto writer = CBufferWriter(reinterpret_cast<uint8_t*>(mapped.pData));
	per_pixel.write(writer);
	context->Unmap(per_pixel_cbuf.Get(), 0);
	per_pixel.clear();
}

void Direct3DDevice8::commit_per_model()
{
	if (!per_model.dirty())
	{
		return;
	}

	D3D11_MAPPED_SUBRESOURCE mapped {};
	context->Map(per_model_cbuf.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);

	auto writer = CBufferWriter(reinterpret_cast<uint8_t*>(mapped.pData));

	per_model.write(writer);
	per_model.clear();

	context->Unmap(per_model_cbuf.Get(), 0);
}

void Direct3DDevice8::commit_per_scene()
{
	per_scene.screen_dimensions = { viewport.Width, viewport.Height };

	if (!per_scene.dirty())
	{
		return;
	}

	D3D11_MAPPED_SUBRESOURCE mapped {};
	context->Map(per_scene_cbuf.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);

	auto writer = CBufferWriter(reinterpret_cast<uint8_t*>(mapped.pData));
	per_scene.write(writer);
	per_scene.clear();
	context->Unmap(per_scene_cbuf.Get(), 0);
}

void Direct3DDevice8::commit_per_texture()
{
	if (!per_texture.dirty())
	{
		return;
	}

	D3D11_MAPPED_SUBRESOURCE mapped {};
	context->Map(per_texture_cbuf.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);

	auto writer = CBufferWriter(reinterpret_cast<uint8_t*>(mapped.pData));
	per_texture.write(writer);
	per_texture.clear();
	context->Unmap(per_texture_cbuf.Get(), 0);
}

void Direct3DDevice8::update_sampler()
{
	for (auto& setting_it : sampler_setting_values)
	{
		auto& setting = setting_it.second;

		if (!setting.dirty())
		{
			continue;
		}

		setting.clear();

		const auto it = sampler_states.find(setting);

		if (it != sampler_states.end())
		{
			context->PSSetSamplers(setting_it.first, 1, it->second.GetAddressOf());
			return;
		}

		D3D11_SAMPLER_DESC sampler_desc {};

		sampler_desc.Filter         = to_d3d11(setting.filter_min, setting.filter_mag, setting.filter_mip);;
		sampler_desc.AddressU       = static_cast<D3D11_TEXTURE_ADDRESS_MODE>(setting.address_u.data());
		sampler_desc.AddressV       = static_cast<D3D11_TEXTURE_ADDRESS_MODE>(setting.address_v.data());
		sampler_desc.AddressW       = static_cast<D3D11_TEXTURE_ADDRESS_MODE>(setting.address_w.data());
		sampler_desc.MinLOD         = -D3D11_FLOAT32_MAX; // TODO: pull from render state values
		sampler_desc.MaxLOD         = D3D11_FLOAT32_MAX;  // TODO: pull from render state values
		sampler_desc.MipLODBias     = 0.0f;               // TODO: pull from render state values
		sampler_desc.MaxAnisotropy  = setting.max_anisotropy;
		sampler_desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
		sampler_desc.BorderColor[0] = 1.0f;
		sampler_desc.BorderColor[1] = 1.0f;
		sampler_desc.BorderColor[2] = 1.0f;
		sampler_desc.BorderColor[3] = 1.0f;

		ComPtr<ID3D11SamplerState> sampler_state;
		HRESULT hr = device->CreateSamplerState(&sampler_desc, &sampler_state);

		if (FAILED(hr))
		{
			throw std::runtime_error("CreateSamplerState failed");
		}

		context->PSSetSamplers(setting_it.first, 1, sampler_state.GetAddressOf());
		sampler_states[setting] = sampler_state;
	}
}

std::optional<PixelShader> Direct3DDevice8::get_ps_async(ShaderFlags::type flags)
{
	flags = ShaderFlags::sanitize(flags & ShaderFlags::ps_mask);

	{
		LOCK(ps_tasks_mutex);

		auto task_it = ps_tasks.find(flags);

		if (task_it != ps_tasks.end())
		{
			auto status = task_it->second.wait_for(std::chrono::milliseconds(0));

			if (status == std::future_status::ready)
			{
				auto result = task_it->second.get();
				ps_tasks.erase(task_it);
				return result;
			}

			return std::nullopt;
		}

		// clean
		for (auto it = ps_tasks.begin(); it != ps_tasks.end();)
		{
			auto status = it->second.wait_for(std::chrono::milliseconds(0));

			if (status == std::future_status::ready)
			{
				if (it->first != flags)
				{
					it = ps_tasks.erase(it);
				}
				else
				{
					auto result = it->second.get();
					ps_tasks.erase(it);
					return result;
				}
			}
			else
			{
				++it;
			}
		}
	}

	{
		LOCK(ps_mutex);

		const auto ps_it = pixel_shaders.find(flags);

		if (ps_it != pixel_shaders.end())
		{
			LOCK(ps_tasks_mutex);
			ps_tasks.erase(ps_it->first);
			return ps_it->second;
		}
	}

	{
		LOCK(ps_tasks_mutex);

		if (ps_tasks.size() >= std::thread::hardware_concurrency())
		{
			//OutputDebugStringA("PS TASK COUNT REACHED HARDWARE LIMIT\n");
			return std::nullopt;
		}

		auto task = std::async(std::launch::async, [this, flags]() -> auto
		{
			return get_ps(flags, false, pixel_shaders, ps_mutex);
		});

		ps_tasks[flags] = std::move(task);
	}

	return std::nullopt;
}

void Direct3DDevice8::compile_shaders(ShaderFlags::type flags, VertexShader& vs, PixelShader& ps)
{
	int result;

	do
	{
		try
		{
			vs = get_vs(flags, false, vertex_shaders, vs_mutex);

		#ifdef SHADER_ASYNC_COMPILE
			auto ps_async = get_ps_async(flags);

			if (ps_async.has_value())
			{
				ps = ps_async.value();
			}
			#ifdef SHADER_FAST_FALLBACK
			else
			{
				ps = get_ps(flags, true, pixel_shaders, ps_speed_mutex);
			}
			#endif
		#else
			ps = get_ps(flags, true, pixel_shaders, ps_speed_mutex);
		#endif

			break;
		}
		catch (std::exception& ex)
		{
			free_shaders();
			result = MessageBoxA(/*WindowHandle*/ nullptr, ex.what(), "Shader compilation error", MB_RETRYCANCEL | MB_ICONERROR);
		}
	} while (result == IDRETRY);
}

void Direct3DDevice8::update_shaders()
{
	if (oit_actually_enabled)
	{
		shader_flags |= ShaderFlags::rs_oit;
	}

	shader_flags &= ~ShaderFlags::stage_count;
	shader_flags |= (static_cast<ShaderFlags::type>(count_texture_stages()) << ShaderFlags::stage_count_shift) & ShaderFlags::stage_count;

	shader_flags = ShaderFlags::sanitize(shader_flags);

	shader_preprocess(shader_flags);

	if (last_shader_flags == shader_flags)
	{
		return;
	}

	VertexShader vs;
	PixelShader ps;

	compile_shaders(shader_flags, vs, ps);

	if ((shader_flags & ShaderFlags::vs_mask) != (last_shader_flags & ShaderFlags::vs_mask))
	{
		context->VSSetShader(vs.shader.Get(), nullptr, 0);
	}

	if ((shader_flags & ShaderFlags::ps_mask) != (last_shader_flags & ShaderFlags::ps_mask))
	{
		context->PSSetShader(ps.shader.Get(), nullptr, 0);
	}

	last_shader_flags = shader_flags;

	if (!ps.has_value())
	{
		last_shader_flags &= ShaderFlags::ps_mask;
	}

	current_vs = vs;
	current_ps = ps;
}

void Direct3DDevice8::update_blend()
{
	if (!blend_flags.dirty())
	{
		return;
	}

	blend_flags.clear();

	const auto it = blend_states.find(blend_flags.data());

	if (it != blend_states.end())
	{
		context->OMSetBlendState(it->second.Get(), nullptr, 0xFFFFFFFF);
		return;
	}

	D3D11_BLEND_DESC desc {};

	const auto flags = blend_flags.data();

	for (auto& rt : desc.RenderTarget)
	{
		rt.BlendEnable           = flags >> 15 & 1;
		rt.SrcBlend              = static_cast<D3D11_BLEND>(flags & 0xF);
		rt.DestBlend             = static_cast<D3D11_BLEND>((flags >> 4) & 0xF);
		rt.BlendOp               = static_cast<D3D11_BLEND_OP>((flags >> 8) & 0xF);
		rt.RenderTargetWriteMask = static_cast<D3D11_COLOR_WRITE_ENABLE>((flags >> BLEND_COLORMASK_SHIFT) & 0xF);
		rt.SrcBlendAlpha         = D3D11_BLEND_ONE;
		rt.DestBlendAlpha        = D3D11_BLEND_ZERO;
		rt.BlendOpAlpha          = D3D11_BLEND_OP_ADD;
	}

	ComPtr<ID3D11BlendState> blend_state;
	HRESULT hr = device->CreateBlendState(&desc, &blend_state);

	if (FAILED(hr))
	{
		throw std::runtime_error("CreateBlendState failed");
	}

	blend_states[flags] = blend_state;
	context->OMSetBlendState(blend_state.Get(), nullptr, 0xFFFFFFFF);
}

void Direct3DDevice8::update_depth()
{
	auto& stencilref = render_state_values[D3DRS_STENCILREF];

	if (!depthstencil_flags.dirty() && !stencilref.dirty())
	{
		return;
	}

	depthstencil_flags.clear();
	stencilref.clear();

	const auto it = depth_states.find(depthstencil_flags);

	if (it != depth_states.end())
	{
		context->OMSetDepthStencilState(it->second.Get(), stencilref.data());
		return;
	}

	D3D11_DEPTH_STENCIL_DESC depth_desc {};

	const auto& flags = depthstencil_flags.flags.data();

	depth_desc.DepthEnable    = !!(flags & DepthStencilFlags::depth_test_enabled);
	depth_desc.DepthWriteMask = (flags & DepthStencilFlags::depth_write_enabled) ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
	depth_desc.StencilEnable  = !!(flags & DepthStencilFlags::stencil_enabled);

	const auto& depth_flags = depthstencil_flags.depth_flags.data();
	depth_desc.DepthFunc = static_cast<D3D11_COMPARISON_FUNC>(depth_flags & DepthFlags::comparison_mask);

	if (depth_desc.StencilEnable)
	{
		const auto& stencil_flags = depthstencil_flags.stencil_flags.data();
		D3D11_DEPTH_STENCILOP_DESC stencil_desc;

		stencil_desc.StencilFailOp      = static_cast<D3D11_STENCIL_OP>((stencil_flags >> StencilFlags::fail_shift) & StencilFlags::op_mask);
		stencil_desc.StencilDepthFailOp = static_cast<D3D11_STENCIL_OP>((stencil_flags >> StencilFlags::zfail_shift) & StencilFlags::op_mask);
		stencil_desc.StencilPassOp      = static_cast<D3D11_STENCIL_OP>((stencil_flags >> StencilFlags::pass_shift) & StencilFlags::op_mask);
		stencil_desc.StencilFunc        = static_cast<D3D11_COMPARISON_FUNC>((stencil_flags >> StencilFlags::func_shift) & StencilFlags::op_mask);

		depth_desc.StencilReadMask  = (stencil_flags >> StencilFlags::read_shift) & StencilFlags::rw_mask;
		depth_desc.StencilWriteMask = (stencil_flags >> StencilFlags::write_shift) & StencilFlags::rw_mask;

		depth_desc.FrontFace = stencil_desc;
		depth_desc.BackFace  = stencil_desc;
	}
	else
	{
		depth_desc.FrontFace = {
			D3D11_STENCIL_OP_KEEP,
			D3D11_STENCIL_OP_KEEP,
			D3D11_STENCIL_OP_KEEP,
			D3D11_COMPARISON_ALWAYS
		};

		depth_desc.BackFace = {
			D3D11_STENCIL_OP_KEEP,
			D3D11_STENCIL_OP_KEEP,
			D3D11_STENCIL_OP_KEEP,
			D3D11_COMPARISON_ALWAYS
		};
	}

	ComPtr<ID3D11DepthStencilState> depth_state;
	if (FAILED(device->CreateDepthStencilState(&depth_desc, &depth_state)))
	{
		throw std::runtime_error("Failed to create depth stencil!");
	}

	context->OMSetDepthStencilState(depth_state.Get(), stencilref.data());
	depth_states[depthstencil_flags] = std::move(depth_state);
}

void Direct3DDevice8::update_rasterizers()
{
	if (!raster_flags.dirty())
	{
		return;
	}

	raster_flags.clear();
	const auto it = raster_states.find(raster_flags);

	if (it != raster_states.end())
	{
		context->RSSetState(it->second.Get());
		return;
	}

	D3D11_RASTERIZER_DESC raster {};

	raster.FillMode        = static_cast<D3D11_FILL_MODE>((raster_flags.data() >> 2) & 3);
	raster.CullMode        = static_cast<D3D11_CULL_MODE>(raster_flags.data() & 3);
	raster.DepthClipEnable = TRUE;

	ComPtr<ID3D11RasterizerState> raster_state;
	if (FAILED(device->CreateRasterizerState(&raster, &raster_state)))
	{
		throw std::runtime_error("failed to create rasterizer state");
	}

	context->RSSetState(raster_state.Get());
	raster_states.emplace(raster_flags, std::move(raster_state));
}

bool Direct3DDevice8::update()
{
	update_rasterizers();
	update_shaders();
	update_sampler();
	update_blend();
	update_depth();
	commit_per_scene();
	commit_per_texture();
	commit_per_model();
	commit_per_pixel();

	if (skip_draw())
	{
		return true;
	}

	return update_input_layout();
}

bool Direct3DDevice8::skip_draw() const
{
	return !current_ps.has_value() || !current_vs.has_value();
}

void Direct3DDevice8::free_shaders()
{
	last_shader_flags = ShaderFlags::mask;

	LOCK(ps_tasks_mutex);
	ps_tasks.clear();

	LOCK(vs_tasks_mutex);
	vs_tasks.clear();

	LOCK(shader_sources_mutex);
	LOCK(vs_mutex);
	LOCK(vs_speed_mutex);
	LOCK(ps_mutex);
	LOCK(ps_speed_mutex);

	current_vs = {};
	current_ps = {};

	shader_sources.clear();
	vertex_shaders.clear();
	pixel_shaders.clear();
	vertex_speed_shaders.clear();
	pixel_speed_shaders.clear();

	fvf_layouts.clear();

	for (auto& value : render_state_values)
	{
		value.mark();
	}

	depthstencil_flags.mark();
	blend_flags.mark();

	for (auto& pair : sampler_setting_values)
	{
		pair.second.mark();
	}

	per_model.mark();
	per_pixel.mark();
	per_scene.mark();
	per_texture.mark();

	composite_vs = {};
	composite_ps = {};
}

void Direct3DDevice8::oit_load_shaders()
{
	D3D_SHADER_MACRO preproc[] = {
		{ "MAX_FRAGMENTS", fragments_str.c_str() },
		{}
	};

	int result;

	do
	{
		try
		{
			ComPtr<ID3DBlob> errors;
			ComPtr<ID3DBlob> blob;

			ShaderIncluder includer;

			constexpr auto path = "composite.hlsl";
			const auto& src = includer.get_shader_source(path);

			HRESULT hr = D3DCompile(src.data(), src.size(), path, &preproc[0], &includer, "vs_main", "vs_5_0", 0, 0, &blob, &errors);

			if (FAILED(hr))
			{
				std::string str(static_cast<char*>(errors->GetBufferPointer()), 0, errors->GetBufferSize());
				throw std::runtime_error(str);
			}

			hr = device->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &composite_vs.shader);

			if (FAILED(hr))
			{
				throw std::runtime_error("composite vertex shader creation failed");
			}

			hr = D3DCompile(src.data(), src.size(), path, &preproc[0], &includer, "ps_main", "ps_5_0", 0, 0, &blob, &errors);

			if (FAILED(hr))
			{
				std::string str(static_cast<char*>(errors->GetBufferPointer()), 0, errors->GetBufferSize());
				throw std::runtime_error(str);
			}

			hr = device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &composite_ps.shader);

			if (FAILED(hr))
			{
				throw std::runtime_error("composite pixel shader creation failed");
			}
			break;
		}
		catch (std::exception& ex)
		{
			print_info_queue();
			free_shaders();
			result = MessageBoxA(nullptr, ex.what(), "Shader compilation error", MB_RETRYCANCEL | MB_ICONERROR);
		}
	} while (result == IDRETRY);
}

void Direct3DDevice8::oit_release()
{
	static std::array<ID3D11UnorderedAccessView*, 5> null = {};

	context->OMSetRenderTargetsAndUnorderedAccessViews(1, render_target_view.GetAddressOf(), nullptr,
	                                                   1, null.size(), &null[0], nullptr);

	FragListHead     = nullptr;
	FragListHeadSRV  = nullptr;
	FragListHeadUAV  = nullptr;
	FragListCount    = nullptr;
	FragListCountSRV = nullptr;
	FragListCountUAV = nullptr;
	FragListNodes    = nullptr;
	FragListNodesSRV = nullptr;
	FragListNodesUAV = nullptr;
}

void Direct3DDevice8::oit_write()
{
	// Unbinds the shader resource views for our fragment list and list head.
	// UAVs cannot be bound as standard resource views and UAVs simultaneously.
	std::array<ID3D11ShaderResourceView*, 5> srvs = {};
	context->PSSetShaderResources(0, srvs.size(), &srvs[0]);

	std::array<ID3D11UnorderedAccessView*, 3> uavs = {
		FragListHeadUAV.Get(),
		FragListCountUAV.Get(),
		FragListNodesUAV.Get()
	};

	// This is used to set the hidden counter of FragListNodes to 0.
	// It only works on FragListNodes, but the number of elements here
	// must match the number of UAVs given.
	static const uint zero[3] = { 0, 0, 0 };

	// Binds our fragment list & list head UAVs for read/write operations.
	context->OMSetRenderTargetsAndUnorderedAccessViews(1, oit_actually_enabled ? composite_view.GetAddressOf() : render_target_view.GetAddressOf(),
	                                                   current_depth_stencil->depth_stencil.Get(), 1, uavs.size(), &uavs[0], &zero[0]);

	// Resets the list head indices to FRAGMENT_LIST_NULL.
	// 4 elements are required as this can be used to clear a texture
	// with 4 color channels, even though our list head only has one.
	static const UINT clear_head[] = { UINT_MAX, UINT_MAX, UINT_MAX, UINT_MAX };
	context->ClearUnorderedAccessViewUint(FragListHeadUAV.Get(), &clear_head[0]);

	static const UINT clear_count[] = { 0, 0, 0, 0 };
	context->ClearUnorderedAccessViewUint(FragListCountUAV.Get(), &clear_count[0]);
}

void Direct3DDevice8::oit_read() const
{
	ID3D11UnorderedAccessView* uavs[3] = {};

	// Unbinds our UAVs.
	context->OMSetRenderTargetsAndUnorderedAccessViews(1, render_target_view.GetAddressOf(), nullptr, 1, 3, &uavs[0], nullptr);

	std::array<ID3D11ShaderResourceView*, 5> srvs = {
		FragListHeadSRV.Get(),
		FragListCountSRV.Get(),
		FragListNodesSRV.Get(),
		composite_srv.Get(),
		current_depth_stencil->depth_srv.Get()
	};

	// Binds the shader resource views of our UAV buffers as read-only.
	context->PSSetShaderResources(0, srvs.size(), &srvs[0]);
}

void Direct3DDevice8::oit_init()
{
	FragListHead_Init();
	FragListCount_Init();
	FragListNodes_Init();

	oit_write();
}

void Direct3DDevice8::FragListHead_Init()
{
	D3D11_TEXTURE2D_DESC desc2D = {};

	desc2D.ArraySize          = 1;
	desc2D.BindFlags          = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
	desc2D.Usage              = D3D11_USAGE_DEFAULT;
	desc2D.Format             = DXGI_FORMAT_R32_UINT;
	desc2D.Width              = static_cast<UINT>(viewport.Width);
	desc2D.Height             = static_cast<UINT>(viewport.Height);
	desc2D.MipLevels          = 1;
	desc2D.SampleDesc.Count   = 1;
	desc2D.SampleDesc.Quality = 0;

	if (FAILED(device->CreateTexture2D(&desc2D, nullptr, &FragListHead)))
	{
		throw;
	}

	D3D11_SHADER_RESOURCE_VIEW_DESC descRV;

	descRV.Format                    = desc2D.Format;
	descRV.ViewDimension             = D3D11_SRV_DIMENSION_TEXTURE2D;
	descRV.Texture2D.MipLevels       = 1;
	descRV.Texture2D.MostDetailedMip = 0;

	if (FAILED(device->CreateShaderResourceView(FragListHead.Get(), &descRV, &FragListHeadSRV)))
	{
		throw;
	}

	D3D11_UNORDERED_ACCESS_VIEW_DESC descUAV;

	descUAV.Format              = desc2D.Format;
	descUAV.ViewDimension       = D3D11_UAV_DIMENSION_TEXTURE2D;
	descUAV.Buffer.FirstElement = 0;
	descUAV.Buffer.NumElements  = static_cast<UINT>(viewport.Width) * static_cast<UINT>(viewport.Height);
	descUAV.Buffer.Flags        = 0;

	if (FAILED(device->CreateUnorderedAccessView(FragListHead.Get(), &descUAV, &FragListHeadUAV)))
	{
		throw;
	}
}

void Direct3DDevice8::FragListCount_Init()
{
	D3D11_TEXTURE2D_DESC desc2D = {};

	desc2D.ArraySize          = 1;
	desc2D.BindFlags          = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
	desc2D.Usage              = D3D11_USAGE_DEFAULT;
	desc2D.Format             = DXGI_FORMAT_R32_UINT;
	desc2D.Width              = static_cast<UINT>(viewport.Width);
	desc2D.Height             = static_cast<UINT>(viewport.Height);
	desc2D.MipLevels          = 1;
	desc2D.SampleDesc.Count   = 1;
	desc2D.SampleDesc.Quality = 0;

	if (FAILED(device->CreateTexture2D(&desc2D, nullptr, &FragListCount)))
	{
		throw;
	}

	D3D11_SHADER_RESOURCE_VIEW_DESC descRV;

	descRV.Format                    = desc2D.Format;
	descRV.ViewDimension             = D3D11_SRV_DIMENSION_TEXTURE2D;
	descRV.Texture2D.MipLevels       = 1;
	descRV.Texture2D.MostDetailedMip = 0;

	if (FAILED(device->CreateShaderResourceView(FragListCount.Get(), &descRV, &FragListCountSRV)))
	{
		throw;
	}

	D3D11_UNORDERED_ACCESS_VIEW_DESC descUAV;

	descUAV.Format              = desc2D.Format;
	descUAV.ViewDimension       = D3D11_UAV_DIMENSION_TEXTURE2D;
	descUAV.Buffer.FirstElement = 0;
	descUAV.Buffer.NumElements  = static_cast<UINT>(viewport.Width) * static_cast<UINT>(viewport.Height);
	descUAV.Buffer.Flags        = 0;

	if (FAILED(device->CreateUnorderedAccessView(FragListCount.Get(), &descUAV, &FragListCountUAV)))
	{
		throw;
	}
}

void Direct3DDevice8::FragListNodes_Init()
{
	D3D11_BUFFER_DESC descBuf = {};

	per_scene.buffer_len = static_cast<UINT>(viewport.Width) * static_cast<UINT>(viewport.Height) * globals::max_fragments;

	descBuf.MiscFlags           = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
	descBuf.BindFlags           = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
	descBuf.ByteWidth           = sizeof(OitNode) * static_cast<UINT>(viewport.Width) * static_cast<UINT>(viewport.Height) * globals::max_fragments;
	descBuf.StructureByteStride = sizeof(OitNode);

	if (FAILED(device->CreateBuffer(&descBuf, nullptr, &FragListNodes)))
	{
		throw;
	}

	D3D11_SHADER_RESOURCE_VIEW_DESC descRV = {};

	descRV.Format             = DXGI_FORMAT_UNKNOWN;
	descRV.ViewDimension      = D3D11_SRV_DIMENSION_BUFFER;
	descRV.Buffer.NumElements = static_cast<UINT>(viewport.Width) * static_cast<UINT>(viewport.Height) * globals::max_fragments;

	if (FAILED(device->CreateShaderResourceView(FragListNodes.Get(), &descRV, &FragListNodesSRV)))
	{
		throw;
	}

	D3D11_UNORDERED_ACCESS_VIEW_DESC descUAV;

	descUAV.Format              = DXGI_FORMAT_UNKNOWN;
	descUAV.ViewDimension       = D3D11_UAV_DIMENSION_BUFFER;
	descUAV.Buffer.FirstElement = 0;
	descUAV.Buffer.NumElements  = static_cast<UINT>(viewport.Width) * static_cast<UINT>(viewport.Height) * globals::max_fragments;
	descUAV.Buffer.Flags        = D3D11_BUFFER_UAV_FLAG_COUNTER;

	if (FAILED(device->CreateUnorderedAccessView(FragListNodes.Get(), &descUAV, &FragListNodesUAV)))
	{
		throw;
	}
}

void Direct3DDevice8::up_get(size_t target_size)
{
	const size_t rounded = round_pow2(target_size);

	for (auto it = up_buffers.begin(); it != up_buffers.end(); ++it)
	{
		if ((*it)->desc8.Size >= rounded && (*it)->desc8.Size < 2 * rounded)
		{
			up_buffer = *it;
			up_buffers.erase(it);
			return;
		}
	}

	//printf(__FUNCTION__ " is allocating (%u rounded to %u bytes, %u total buffers)\n", target_size, rounded, up_buffers.size() + 1);
	CreateVertexBuffer(rounded, D3DUSAGE_DYNAMIC, FVF.data(), D3DPOOL_MANAGED, &up_buffer);
}
