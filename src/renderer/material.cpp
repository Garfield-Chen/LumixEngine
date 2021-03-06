#include "renderer/material.h"
#include "core/crc32.h"
#include "core/fs/file_system.h"
#include "core/fs/ifile.h"
#include "core/json_serializer.h"
#include "core/log.h"
#include "core/path_utils.h"
#include "core/profiler.h"
#include "core/resource_manager.h"
#include "core/resource_manager_base.h"
#include "renderer/material_manager.h"
#include "renderer/pipeline.h"
#include "renderer/renderer.h"
#include "renderer/shader.h"
#include "renderer/texture.h"


namespace Lumix
{


static const uint32 SHADOWMAP_HASH = crc32("shadowmap");
static const float DEFAULT_ALPHA_REF_VALUE = 0.3f;


Material::Material(const Path& path, ResourceManager& resource_manager, IAllocator& allocator)
	: Resource(path, resource_manager, allocator)
	, m_shader(nullptr)
	, m_uniforms(allocator)
	, m_allocator(allocator)
	, m_texture_count(0)
	, m_render_states(0)
	, m_color(1, 1, 1)
	, m_shininess(4)
	, m_shader_instance(nullptr)
	, m_define_mask(0)
	, m_command_buffer(nullptr)
	, m_layer_count(1)
{
	auto* manager = resource_manager.get(ResourceManager::MATERIAL);
	auto* mat_manager = static_cast<MaterialManager*>(manager);

	setAlphaRef(DEFAULT_ALPHA_REF_VALUE);
	for (int i = 0; i < MAX_TEXTURE_COUNT; ++i)
	{
		m_textures[i] = nullptr;
	}

	setShader(nullptr);
}


Material::~Material()
{
	ASSERT(isEmpty());
}


bool Material::isDefined(uint8 define_idx) const
{
	return (m_define_mask & (1 << define_idx)) != 0;
}


bool Material::hasDefine(uint8 define_idx) const
{
	return m_shader->hasDefine(define_idx) != 0;
}


void Material::setDefine(uint8 define_idx, bool enabled)
{
	uint32 old_mask = m_define_mask;
	if (enabled)
	{
		m_define_mask |= 1 << define_idx;
	}
	else
	{
		m_define_mask &= ~(1 << define_idx);
	}

	if (!isReady()) return;
	if (!m_shader) return;

	if (old_mask != m_define_mask)
	{
		m_shader_instance = &m_shader->getInstance(m_define_mask);
	}
}


void Material::unload(void)
{
	m_allocator.deallocate(m_command_buffer);
	m_command_buffer = nullptr;
	m_uniforms.clear();
	setShader(nullptr);

	ResourceManagerBase* texture_manager = m_resource_manager.get(ResourceManager::TEXTURE);
	for (int i = 0; i < m_texture_count; i++)
	{
		if (m_textures[i])
		{
			removeDependency(*m_textures[i]);
			texture_manager->unload(*m_textures[i]);
		}
	}
	m_texture_count = 0;
}


bool Material::save(JsonSerializer& serializer)
{
	if(!isReady()) return false;
	if(!m_shader) return false;

	auto* manager = getResourceManager().get(ResourceManager::MATERIAL);
	auto& renderer = static_cast<MaterialManager*>(manager)->getRenderer();

	serializer.beginObject();
	serializer.serialize("shader", m_shader ? m_shader->getPath() : Path(""));
	if (m_layer_count != 1)serializer.serialize("layer_count", m_layer_count);
	for (int i = 0; i < m_texture_count; ++i)
	{
		char path[MAX_PATH_LENGTH];
		int flags = 0;
		int atlas_size = -1;
		if (m_textures[i])
		{
			flags = m_textures[i]->getFlags();
			path[0] = '/';
			Lumix::copyString(path + 1, MAX_PATH_LENGTH - 1, m_textures[i]->getPath().c_str());
			atlas_size = m_textures[i]->getAtlasSize();
		}
		else
		{
			path[0] = '\0';
		}
		serializer.beginObject("texture");
		serializer.serialize("source", path);
		if (atlas_size > 0) serializer.serialize("atlas_size", atlas_size);
		if (flags & BGFX_TEXTURE_SRGB) serializer.serialize("srgb", true);
		if (flags & BGFX_TEXTURE_U_CLAMP) serializer.serialize("u_clamp", true);
		if (flags & BGFX_TEXTURE_V_CLAMP) serializer.serialize("v_clamp", true);
		if (flags & BGFX_TEXTURE_W_CLAMP) serializer.serialize("w_clamp", true);
		if (flags & BGFX_TEXTURE_MIN_POINT) serializer.serialize("min_filter", "point");
		if (flags & BGFX_TEXTURE_MIN_ANISOTROPIC) serializer.serialize("min_filter", "anisotropic");
		if (flags & BGFX_TEXTURE_MAG_POINT) serializer.serialize("mag_filter", "point");
		if (flags & BGFX_TEXTURE_MAG_ANISOTROPIC) serializer.serialize("mag_filter", "anisotropic");
		if (m_textures[i] && m_textures[i]->getData()) serializer.serialize("keep_data", true);
		serializer.endObject();
	}

	serializer.beginArray("defines");
	for (int i = 0; i < sizeof(m_define_mask) * 8; ++i)
	{
		if (m_define_mask & (1 << i)) serializer.serializeArrayItem(renderer.getShaderDefine(i));
	}
	serializer.endArray();

	serializer.beginArray("uniforms");
	for (int i = 0; i < m_shader->getUniformCount(); ++i)
	{
		serializer.beginObject();
		const auto& uniform = m_shader->getUniform(i);

		serializer.serialize("name", uniform.name);
		switch (uniform.type)
		{
			case Shader::Uniform::FLOAT:
				serializer.serialize("float_value", m_uniforms[i].float_value);
				break;
			case Shader::Uniform::COLOR:
				serializer.beginArray("color");
					serializer.serializeArrayItem(m_uniforms[i].vec3[0]);
					serializer.serializeArrayItem(m_uniforms[i].vec3[1]);
					serializer.serializeArrayItem(m_uniforms[i].vec3[2]);
				serializer.endArray();
				break;
			case Shader::Uniform::VEC3:
				serializer.beginArray("vec3");
					serializer.serializeArrayItem(m_uniforms[i].vec3[0]);
					serializer.serializeArrayItem(m_uniforms[i].vec3[1]);
					serializer.serializeArrayItem(m_uniforms[i].vec3[2]);
				serializer.endArray();
				break;
			case Shader::Uniform::TIME:
				serializer.serialize("time", 0);
				break;
			case Shader::Uniform::INT:
				serializer.serialize("int_value", m_uniforms[i].int_value);
				break;
			case Shader::Uniform::MATRIX4:
				serializer.beginArray("matrix_value");
				for (int j = 0; j < 16; ++j)
				{
					serializer.serializeArrayItem(m_uniforms[i].matrix[j]);
				}
				serializer.endArray();
				break;
			default:
				ASSERT(false);
				break;
		}
		serializer.endObject();
	}
	serializer.endArray();
	serializer.serialize("shininess", m_shininess);
	serializer.serialize("alpha_ref", m_alpha_ref);
	serializer.beginArray("color");
		serializer.serializeArrayItem(m_color.x);
		serializer.serializeArrayItem(m_color.y);
		serializer.serializeArrayItem(m_color.z);
	serializer.endArray();
	serializer.endObject();
	return true;
}


void Material::deserializeDefines(JsonSerializer& serializer)
{
	auto* manager = getResourceManager().get(ResourceManager::MATERIAL);
	auto& renderer = static_cast<MaterialManager*>(manager)->getRenderer();
	serializer.deserializeArrayBegin();
	m_define_mask = 0;
	while (!serializer.isArrayEnd())
	{
		char tmp[32];
		serializer.deserializeArrayItem(tmp, lengthOf(tmp), "");
		m_define_mask |= 1 << renderer.getShaderDefineIdx(tmp);
	}
	serializer.deserializeArrayEnd();
}


void Material::deserializeUniforms(JsonSerializer& serializer)
{
	serializer.deserializeArrayBegin();
	m_uniforms.clear();
	while (!serializer.isArrayEnd())
	{
		Uniform& uniform = m_uniforms.emplace();
		serializer.nextArrayItem();
		serializer.deserializeObjectBegin();
		char label[256];
		auto uniform_type = bgfx::UniformType::End;
		while (!serializer.isObjectEnd())
		{
			serializer.deserializeLabel(label, 255);
			if (compareString(label, "name") == 0)
			{
				char name[32];
				serializer.deserialize(name, lengthOf(name), "");
				uniform.name_hash = crc32(name);
			}
			else if (compareString(label, "int_value") == 0)
			{
				serializer.deserialize(uniform.int_value, 0);
			}
			else if (compareString(label, "float_value") == 0)
			{
				serializer.deserialize(uniform.float_value, 0);
			}
			else if (compareString(label, "matrix_value") == 0)
			{
				serializer.deserializeArrayBegin();
				for (int i = 0; i < 16; ++i)
				{
					serializer.deserializeArrayItem(uniform.matrix[i], 0);
				}
				serializer.deserializeArrayEnd();
			}
			else if (compareString(label, "time") == 0)
			{
				serializer.deserialize(uniform.float_value, 0);
			}
			else if (compareString(label, "color") == 0)
			{
				serializer.deserializeArrayBegin();
					serializer.deserializeArrayItem(uniform.vec3[0], 0);
					serializer.deserializeArrayItem(uniform.vec3[1], 0);
					serializer.deserializeArrayItem(uniform.vec3[2], 0);
				serializer.deserializeArrayEnd();
			}
			else if (compareString(label, "vec3") == 0)
			{
				serializer.deserializeArrayBegin();
					serializer.deserializeArrayItem(uniform.vec3[0], 0);
					serializer.deserializeArrayItem(uniform.vec3[1], 0);
					serializer.deserializeArrayItem(uniform.vec3[2], 0);
				serializer.deserializeArrayEnd();
			}
			else
			{
				g_log_warning.log("Renderer") << "Unknown label \"" << label << "\"";
			}
		}
		serializer.deserializeObjectEnd();
	}
	serializer.deserializeArrayEnd();
}


void Material::setTexturePath(int i, const Path& path)
{
	if (path.length() == 0)
	{
		setTexture(i, nullptr);
	}
	else
	{
		Texture* texture =
			static_cast<Texture*>(m_resource_manager.get(ResourceManager::TEXTURE)->load(path));
		setTexture(i, texture);
	}
}


void Material::setTexture(int i, Texture* texture)
{
	Texture* old_texture = i < m_texture_count ? m_textures[i] : nullptr;

	if (texture) addDependency(*texture);
	m_textures[i] = texture;
	if (i >= m_texture_count) m_texture_count = i + 1;

	if (old_texture)
	{
		if (texture) texture->setAtlasSize(old_texture->getAtlasSize());
		removeDependency(*old_texture);
		m_resource_manager.get(ResourceManager::TEXTURE)->unload(*old_texture);
	}
	if (isReady() && m_shader)
	{
		int define_idx = m_shader->getTextureSlot(i).m_define_idx;
		if(define_idx >= 0)
		{
			if(m_textures[i])
			{
				m_define_mask |= 1 << define_idx;
			}
			else
			{
				m_define_mask &= ~(1 << define_idx);
			}
		}

		createCommandBuffer();
		m_shader_instance = &m_shader->getInstance(m_define_mask);
	}
}


void Material::setShader(const Path& path)
{
	Shader* shader = static_cast<Shader*>(m_resource_manager.get(ResourceManager::SHADER)->load(path));
	setShader(shader);
}


void Material::createCommandBuffer()
{
	m_allocator.deallocate(m_command_buffer);
	m_command_buffer = nullptr;
	if (!m_shader) return;

	CommandBufferGenerator generator;

	for (int i = 0; i < m_shader->getUniformCount(); ++i)
	{
		const Material::Uniform& uniform = m_uniforms[i];
		const Shader::Uniform& shader_uniform = m_shader->getUniform(i);

		switch (shader_uniform.type)
		{
			case Shader::Uniform::FLOAT:
				generator.setUniform(shader_uniform.handle, Vec4(uniform.float_value, 0, 0, 0));
				break;
			case Shader::Uniform::VEC3:
			case Shader::Uniform::COLOR:
				generator.setUniform(shader_uniform.handle, Vec4(*(Vec3*)uniform.vec3, 0));
				break;
			case Shader::Uniform::TIME: generator.setTimeUniform(shader_uniform.handle); break;
			default: ASSERT(false); break;
		}
	}

	for (int i = 0; i < m_shader->getTextureSlotCount(); ++i)
	{
		if (i >= m_texture_count || !m_textures[i]) continue;

		generator.setTexture(
			i, m_shader->getTextureSlot(i).m_uniform_handle, m_textures[i]->getTextureHandle());
	}

	Vec4 color_shininess(m_color, m_shininess);
	auto* material_manager = getResourceManager().get(ResourceManager::MATERIAL);
	auto& renderer = static_cast<MaterialManager*>(material_manager)->getRenderer();
	auto& uniform = renderer.getMaterialColorShininessUniform();
	generator.setUniform(uniform, color_shininess);
	generator.end();

	m_command_buffer = (uint8*)m_allocator.allocate(generator.getSize());
	generator.getData(m_command_buffer);
}


void Material::onBeforeReady()
{
	if (!m_shader) return;

	for(int i = 0; i < m_shader->getUniformCount(); ++i)
	{
		auto& shader_uniform = m_shader->getUniform(i);
		bool found = false;
		for(int j = i; j < m_uniforms.size(); ++j)
		{
			if(m_uniforms[j].name_hash == shader_uniform.name_hash)
			{
				auto tmp = m_uniforms[i];
				m_uniforms[i] = m_uniforms[j];
				m_uniforms[j] = tmp;
				found = true;
				break;
			}
		}
		if(found) continue;
		if(i < m_uniforms.size())
		{
			m_uniforms.emplace(m_uniforms[i]);
		}
		else
		{
			m_uniforms.emplace();
		}
		m_uniforms[i].name_hash = shader_uniform.name_hash;
	}

	uint8 alpha_ref = uint8(m_alpha_ref * 255.0f);
	m_render_states = BGFX_STATE_ALPHA_REF(alpha_ref);
	m_render_states |= m_shader->m_render_states;

	for(int i = 0; i < m_shader->getTextureSlotCount(); ++i)
	{
		int define_idx = m_shader->getTextureSlot(i).m_define_idx;
		if(define_idx >= 0)
		{
			if(m_textures[i])
			{
				m_define_mask |= 1 << define_idx;
			}
			else
			{
				m_define_mask &= ~(1 << define_idx);
			}
		}
	}

	createCommandBuffer();
	m_shader_instance = &m_shader->getInstance(m_define_mask);
}


void Material::setShader(Shader* shader)
{
	auto* manager = getResourceManager().get(ResourceManager::MATERIAL);
	auto* mat_manager = static_cast<MaterialManager*>(manager);

	if (m_shader && m_shader != mat_manager->getRenderer().getDefaultShader())
	{
		Shader* shader = m_shader;
		m_shader = nullptr;
		removeDependency(*shader);
		m_resource_manager.get(ResourceManager::SHADER)->unload(*shader);
	}
	m_shader = shader;
	if (m_shader)
	{
		addDependency(*m_shader);
		if (m_shader->isReady()) onBeforeReady();
	}
	else
	{
		m_shader = mat_manager->getRenderer().getDefaultShader();
		m_shader_instance = m_shader->getFirstInstance();
	}
}


const char* Material::getTextureUniform(int i)
{
	if (i < m_shader->getTextureSlotCount())
	{
		return m_shader->getTextureSlot(i).m_uniform;
	}
	return "";
}


Texture* Material::getTextureByUniform(const char* uniform) const
{
	if (!m_shader)
	{
		return nullptr;
	}

	for (int i = 0, c = m_shader->getTextureSlotCount(); i < c; ++i)
	{
		if (compareString(m_shader->getTextureSlot(i).m_uniform, uniform) == 0)
		{
			return m_textures[i];
		}
	}
	return nullptr;
}

bool Material::deserializeTexture(JsonSerializer& serializer, const char* material_dir)
{
	char path[MAX_PATH_LENGTH];
	serializer.deserializeObjectBegin();
	char label[256];
	bool keep_data = false;
	uint32 flags = 0;
	int atlas_size = -1;

	while (!serializer.isObjectEnd())
	{
		serializer.deserializeLabel(label, sizeof(label));
		if (compareString(label, "source") == 0)
		{
			serializer.deserialize(path, MAX_PATH_LENGTH, "");
			if (path[0] != '\0')
			{
				char texture_path[MAX_PATH_LENGTH];
				if (path[0] != '/' && path[0] != '\\')
				{
					copyString(texture_path, material_dir);
					catString(texture_path, path);
				}
				else
				{
					copyString(texture_path, path);
				}
				auto* mng = m_resource_manager.get(ResourceManager::TEXTURE);
				m_textures[m_texture_count] = static_cast<Texture*>(mng->load(Path(texture_path)));
				addDependency(*m_textures[m_texture_count]);
			}
		}
		else if (compareString(label, "atlas_size") == 0)
		{
			serializer.deserialize(atlas_size, -1);
		}
		else if (compareString(label, "min_filter") == 0)
		{
			serializer.deserialize(label, sizeof(label), "");
			if (compareString(label, "point") == 0)
			{
				flags |= BGFX_TEXTURE_MIN_POINT;
			}
			else if (compareString(label, "anisotropic") == 0)
			{
				flags |= BGFX_TEXTURE_MIN_ANISOTROPIC;
			}
			else
			{
				g_log_error.log("Renderer") << "Unknown texture filter \"" << label
											<< "\" in material " << getPath();
			}
		}
		else if (compareString(label, "mag_filter") == 0)
		{
			serializer.deserialize(label, sizeof(label), "");
			if (compareString(label, "point") == 0)
			{
				flags |= BGFX_TEXTURE_MAG_POINT;
			}
			else if (compareString(label, "anisotropic") == 0)
			{
				flags |= BGFX_TEXTURE_MAG_ANISOTROPIC;
			}
			else
			{
				g_log_error.log("Renderer") << "Unknown texture filter \"" << label
											<< "\" in material " << getPath();
			}
		}
		else if (compareString(label, "u_clamp") == 0)
		{
			bool b;
			serializer.deserialize(b, false);
			if (b)
			{
				flags |= BGFX_TEXTURE_U_CLAMP;
			}
		}
		else if (compareString(label, "v_clamp") == 0)
		{
			bool b;
			serializer.deserialize(b, false);
			if (b)
			{
				flags |= BGFX_TEXTURE_V_CLAMP;
			}
		}
		else if (compareString(label, "w_clamp") == 0)
		{
			bool b;
			serializer.deserialize(b, false);
			if (b)
			{
				flags |= BGFX_TEXTURE_W_CLAMP;
			}
		}
		else if (compareString(label, "keep_data") == 0)
		{
			serializer.deserialize(keep_data, false);
		}
		else if (compareString(label, "srgb") == 0)
		{
			bool is_srgb;
			serializer.deserialize(is_srgb, false);
			if(is_srgb) flags |= BGFX_TEXTURE_SRGB;
		}
		else
		{
			g_log_warning.log("Renderer") << "Unknown data \"" << label << "\" in material "
										  << getPath();
			return false;
		}
	}
	if (m_textures[m_texture_count])
	{
		m_textures[m_texture_count]->setAtlasSize(atlas_size);
		m_textures[m_texture_count]->setFlags(flags);

		if (keep_data)
		{
			m_textures[m_texture_count]->addDataReference();
		}
	}
	serializer.deserializeObjectEnd();
	++m_texture_count;
	return true;
}


void Material::setAlphaRef(float value)
{
	m_alpha_ref = value;
	uint8 val = uint8(value * 255.0f);
	m_render_states &= ~BGFX_STATE_ALPHA_REF_MASK;
	m_render_states |= BGFX_STATE_ALPHA_REF(val);
}


bool Material::load(FS::IFile& file)
{
	PROFILE_FUNCTION();

	m_render_states = 0;
	setAlphaRef(DEFAULT_ALPHA_REF_VALUE);
	m_uniforms.clear();
	JsonSerializer serializer(file, JsonSerializer::READ, getPath(), m_allocator);
	serializer.deserializeObjectBegin();
	char label[256];
	char material_dir[MAX_PATH_LENGTH];
	PathUtils::getDir(material_dir, MAX_PATH_LENGTH, getPath().c_str());
	while (!serializer.isObjectEnd())
	{
		serializer.deserializeLabel(label, 255);
		if (compareString(label, "defines") == 0)
		{
			deserializeDefines(serializer);
		}
		else if (compareString(label, "uniforms") == 0)
		{
			deserializeUniforms(serializer);
		}
		else if (compareString(label, "texture") == 0)
		{
			if (!deserializeTexture(serializer, material_dir))
			{
				return false;
			}
		}
		else if (compareString(label, "alpha_ref") == 0)
		{
			serializer.deserialize(m_alpha_ref, 0.3f);
		}
		else if (compareString(label, "layer_count") == 0)
		{
			serializer.deserialize(m_layer_count, 1);
		}
		else if (compareString(label, "color") == 0)
		{
			serializer.deserializeArrayBegin();
			serializer.deserializeArrayItem(m_color.x, 1.0f);
			serializer.deserializeArrayItem(m_color.y, 1.0f);
			serializer.deserializeArrayItem(m_color.z, 1.0f);
			serializer.deserializeArrayEnd();
		}
		else if (compareString(label, "shininess") == 0)
		{
			serializer.deserialize(m_shininess, 4.0f);
		}
		else if (compareString(label, "shader") == 0)
		{
			Path path;
			serializer.deserialize(path, Path(""));
			auto* manager = m_resource_manager.get(ResourceManager::SHADER);
			setShader(static_cast<Shader*>(manager->load(Path(path))));
		}
		else
		{
			g_log_warning.log("Renderer") << "Unknown parameter " << label << " in material "
										  << getPath();
		}
	}
	serializer.deserializeObjectEnd();

	if (!m_shader)
	{
		g_log_error.log("Renderer") << "Material " << getPath() << " without a shader";
		return false;
	}

	m_size = file.size();
	return true;
}


} // ~namespace Lumix
