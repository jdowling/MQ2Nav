#pragma once

#include <glm.hpp>
#include <vector>

struct ModelEntry
{
public:
	struct Poly
	{
		union {
			struct {
				uint32_t v1, v2, v3;
			};
			uint32_t v[3];
		};
		uint8_t vis;
	};
	std::vector<glm::vec3> verts;
	std::vector<Poly> polys;
};