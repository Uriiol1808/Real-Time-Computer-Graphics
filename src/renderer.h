#pragma once
#include "prefab.h"
#include "shader.h"
#include "scene.h"
#include "application.h"
#include "fbo.h"
#include "material.h"
#include "sphericalharmonics.h"
#include "mesh.h"

//forward declarations
class Camera;
class Shader;

namespace GTR {

	class Prefab;
	class Material;
	
	class RenderCall {
	public:
		Matrix44 model;
		Mesh* mesh;
		Material* material;
		BoundingBox world_bounding;

		float distance_to_camera;
	};

	struct sort_distance {
		inline bool operator() (const RenderCall& rc1, const RenderCall& rc2)
		{
			return (rc1.distance_to_camera > rc2.distance_to_camera);
		}
	};

	//struct to store probes
	struct sProbe {
		Vector3 pos; //where is located
		Vector3 local; //its ijk pos in the matrix
		int index; //its index in the linear array
		SphericalHarmonics sh; //coeffs
	};
	
	// This class is in charge of rendering anything in our system.
	// Separating the render from anything else makes the code cleaner
	class Renderer
	{

	public:
		enum eLightMode {
			SINGLE,
			MULTI
		};

		enum ePipeline{
			FORWARD,
			DEFERRED
		};
		
		std::vector<GTR::LightEntity*> lights;
		std::vector<RenderCall> render_calls;

		eLightMode light_mode;
		ePipeline pipeline;
		bool render_shadowmaps;

		//GBUFFERS
		FBO* gbuffers_fbo;
		FBO* illumination_fbo;
		bool show_gbuffers;

		//SSAO
		FBO* ssao_fbo;
		bool show_ssao;
		bool ssao_plus;
		std::vector<Vector3> random_points;

		//HDR - TONE MAPPING
		float u_scale;
		float u_average_lum;
		float u_lumwhite2;
		float u_igamma;
		bool show_hdr;

		//PROBES
		FBO* irr_fbo;
		std::vector<sProbe> probes;
		Texture* probes_texture;
		bool show_probes;
		bool show_probes_texture;
		Vector3 start_irr;
		Vector3 end_irr;
		Vector3 dim_irr;
		Vector3 delta_irr;

		//SKYBOX - REFLECTIONS
		Texture* skybox;
		FBO* reflection_fbo;
		bool is_rendering_reflections;
		FBO* reflection_probe_fbo;
		ReflectionProbeEntity* probe;

		//VOLUMETRIC
		FBO* volumetric_fbo;
		LightEntity* direct_light;

		//DECALS
		std::vector<GTR::DecalEntity*> decals;
		FBO* decals_fbo;
		Mesh cube;

		//POSTFX
		Texture* postFX_textureA;
		Texture* postFX_textureB;
		Texture* postFX_textureC;
		Texture* postFX_textureD;

		float contrast;
		float saturation;
		float vigneting;
		float blur;
		float mix_factor;
		float threshold; 
		Matrix44 vp_matrix_last;

		float minDistance;
		float maxDistance;
		float distortion;
		float noise_amount;
		float chroma;

		float bloom_threshold;
		float bloom_soft_threshold;

		static const int max_lights = 10;
		
		//add here your functions
		//...

		Renderer();

		//renders several elements of the scene
		void renderScene(GTR::Scene* scene, Camera* camera);
	
		//to render a whole prefab (with all its nodes)
		void renderPrefab(const Matrix44& model, GTR::Prefab* prefab, Camera* camera);

		//to render one node from the prefab and its children
		void renderNode(const Matrix44& model, GTR::Node* node, Camera* camera);

		//to render one mesh given its material and transformation matrix
		void renderMeshWithMaterialToGBuffers(const Matrix44 model, Mesh* mesh, GTR::Material* material, Camera* camera);
		void renderMeshWithMaterialandLighting(const Matrix44 model, Mesh* mesh, GTR::Material* material, Camera* camera);
		void renderFlatMesh(const Matrix44 model, Mesh* mesh, GTR::Material* material, Camera* camera);

		//to render the object once
		void renderSinglePass(Shader* shader, Mesh* mesh);

		//to render the object several times, once with every light, and accumulate the result using blending
		void renderMultiPass(Shader* shader, Mesh* mesh, Material* material);
		
		//Shadows
		void uploadLightToShader(GTR::LightEntity* light, Shader* shader);
		void generateShadowmap(LightEntity* light);
		void showShadowmap(LightEntity* light);

		void renderForward(Camera* camera, GTR::Scene* scene);
		void renderDeferred(Camera* camera, GTR::Scene* scene);
		void GbuffersShader(Shader* shader, Scene* scene, Camera* camera);

		void renderProbe(Vector3 pos, float size, float* coeffs);
		void captureProbe(sProbe& probe, GTR::Scene* scene);
		void generateProbes(Scene* scene);

		void renderSkybox(Camera* camera);
		void renderSceneForward(GTR::Scene* scene, Camera* camera);

		void renderReflectionProbes(GTR::Scene* scene, Camera* camera);
		void updateReflectionProbes(GTR::Scene* scene);
		void captureReflectionProbe(GTR::Scene* scene, Texture* texture, Vector3 pos);

		void applyFX(Texture* color_texture, Texture* depth_texture, Camera* camera);

	};

	Texture* CubemapFromHDRE(const char* filename);

	std::vector<Vector3> generateSpherePoints(int num, float radius, bool hemi);
};