#pragma once


#include <Windows.h>
#include <gl/GL.h>
#include "core/string.h"


namespace Lux
{
namespace FS
{
	class FileSystem;
}


class Texture
{
	public:
		Texture();
		~Texture();

		bool create(int w, int h);
		bool load(const char* path, FS::FileSystem& file_system);
		void apply(int unit = 0);

	private:
		void loaded(FS::IFile* file, bool success, FS::FileSystem& fs);

	private:
		GLuint m_id;
		string m_path;
};


} // ~namespace Lux