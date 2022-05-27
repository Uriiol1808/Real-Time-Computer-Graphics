#include "renderer.h"

#include "camera.h"
#include "shader.h"
#include "mesh.h"
#include "texture.h"
#include "prefab.h"
#include "material.h"
#include "utils.h"
#include "scene.h"
#include "extra/hdre.h"
#include "fbo.h"
#include <algorithm>
#include <vector>

using namespace GTR;

Renderer::Renderer()
{
	light_mode = eLightMode::MULTI;
	pipeline = ePipeline::FORWARD;

	render_shadowmaps = false;

	//GBUFFERS
	gbuffers_fbo = NULL;
	illumination_fbo = NULL;
	show_gbuffers = false;

	//SSAO
	ssao_fbo = NULL;
	random_points = generateSpherePoints(64, 1, false);
	show_ssao = false;

	//HDR - TONE MAPPING (Random numbers)
	u_scale = 1.0;
	u_average_lum = 2.5;
	u_lumwhite2 = 10.0;
	u_igamma = 2.2;
}

void Renderer::renderScene(GTR::Scene* scene, Camera* camera)
{
	lights.clear();
	render_calls.clear();

	//Collect info from entities
	for (int i = 0; i < scene->entities.size(); ++i)
	{
		BaseEntity* ent = scene->entities[i];
		if (!ent->visible)
			continue;

		//is a prefab!
		if (ent->entity_type == PREFAB)
		{
			PrefabEntity* pent = (GTR::PrefabEntity*)ent;
			if(pent->prefab)
				renderPrefab(ent->model, pent->prefab, camera);
		}

		//is a light!
		if (ent->entity_type == LIGHT)
		{
			LightEntity* light = (GTR::LightEntity*)ent;
			lights.push_back(light);
		}
	}

	std::sort(render_calls.begin(), render_calls.end(), sort_distance());

	//Generate shadowmaps
	for (int i = 0; i < lights.size(); ++i)
	{
		LightEntity* light = lights[i];
		if (light->cast_shadows)
			generateShadowmap(light);
	}
	
	if (pipeline == FORWARD)
		renderForward(camera, scene);
	else if (pipeline == DEFERRED)
		renderDeferred(camera, scene);

	if (render_shadowmaps)
	{
		int y = Application::instance->window_width - 256;
		for (int i = 0; i < lights.size(); ++i)
		{
			if (!lights[i]->cast_shadows)
				continue;
			glViewport(y, 0, 256, 256);
			showShadowmap(lights[i]);
			y -= 256;
		}
		glViewport(0, 0, Application::instance->window_width, Application::instance->window_height);
	}

}

void GTR::Renderer::renderForward(Camera* camera, GTR::Scene* scene)
{
	//set the clear color (the background color)
	glClearColor(scene->background_color.x, scene->background_color.y, scene->background_color.z, 1.0);

	// Clear the color and the depth buffer
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	checkGLErrors();

	for (int i = 0; i < render_calls.size(); ++i)
	{
		RenderCall& rc = render_calls[i];
		if (camera->testBoxInFrustum(rc.world_bounding.center, rc.world_bounding.halfsize))
			renderMeshWithMaterialandLighting(rc.model, rc.mesh, rc.material, camera);
	}
}

void GTR::Renderer::renderDeferred(Camera* camera, GTR::Scene* scene)
{
	int width = Application::instance->window_width;
	int height = Application::instance->window_height;

	Mesh* quad = Mesh::getQuad(); //2 triangulos que forman un cuadraro en clip space (-1,1 a 1,1)
	Matrix44 inv_vp = camera->viewprojection_matrix;
	inv_vp.inverse();

	//create and FBO
	if (!gbuffers_fbo){
		gbuffers_fbo = new FBO();

		//create 3 textures of 4 components
		gbuffers_fbo->create(width, height,
			3, 			//three textures
			GL_RGBA, 		//four channels
			GL_UNSIGNED_BYTE, //1 byte
			true);		//add depth_texture	
	}

	gbuffers_fbo->bind();

	//set the clear color (the background color)
	glClearColor(scene->background_color.x, scene->background_color.y, scene->background_color.z, 1.0);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

	//Renderizar cada objecto con un GBUffer shader
	for (int i = 0; i < render_calls.size(); ++i)
	{
		RenderCall& rc = render_calls[i];
		if (camera->testBoxInFrustum(rc.world_bounding.center, rc.world_bounding.halfsize))
			renderMeshWithMaterialToGBuffers(rc.model, rc.mesh, rc.material, camera);
	}

	gbuffers_fbo->unbind();

	//SSAO
	if (!ssao_fbo) {
		ssao_fbo = new FBO();

		ssao_fbo->create(width, height,
			1,
			GL_LUMINANCE,
			GL_UNSIGNED_BYTE,
			false);
	}

	ssao_fbo->bind();

	glDisable(GL_DEPTH_TEST);
	glDisable(GL_BLEND);
	Shader* shader = Shader::Get("ssao");
	shader->enable();
	shader->setUniform("u_gb1_texture", gbuffers_fbo->color_textures[1], 1);
	shader->setUniform("u_depth_texture", gbuffers_fbo->depth_texture, 0);
	shader->setUniform("viewprojection", camera->viewprojection_matrix);
	shader->setUniform("u_inverse_viewprojection", inv_vp);
	shader->setUniform("u_iRes", Vector2(1.0 / (float)width, 1.0 / (float)height));
	shader->setUniform3Array("u_points", (float*)&random_points[0], random_points.size());

	quad->render(GL_TRIANGLES);

	ssao_fbo->unbind();

	//ILLUMINATION
	if (!illumination_fbo) {

		illumination_fbo = new FBO();
		//create 1 textures of 3 components
		illumination_fbo->create(width, height,
			1, 			//one texture
			GL_RGB, 		//three channels
			GL_UNSIGNED_BYTE, //1 byte
			false);		//add depth_texture
	}

	illumination_fbo->bind();

	glDisable(GL_DEPTH_TEST);

	//we need a fullscreen quad
	Mesh* sphere = Mesh::Get("data/meshes/sphere.obj", false, false);

	shader = Shader::Get("deferred");
	shader->enable();

	DeferredUniforms(shader, scene, camera, width, height);
	shader->setUniform("u_camera_position", camera->eye);
	shader->setUniform("u_ambient_light", scene->ambient_light);
	shader->setUniform("u_inverse_viewprojection", inv_vp);
	shader->setUniform("u_iRes", Vector2(1.0 / (float)width, 1.0 / (float)height));

	if (!lights.size())
	{
		shader->setUniform("u_light_color", Vector3());
		quad->render(GL_TRIANGLES);
		//sphere->render(GL_TRIANGLES);
	}
	else
		for (int i = 0; i < lights.size(); ++i)
		{
			LightEntity* light = lights[i];

			if (i == 0)
				glDisable(GL_BLEND);
			else
				glBlendFunc(GL_SRC_ALPHA, GL_ONE);
				glEnable(GL_BLEND);

			uploadLightToShader(light, shader);

			//do the draw call that renders the mesh into the screen
			quad->render(GL_TRIANGLES);
			//sphere->render(GL_TRIANGLES);
			shader->setUniform("u_ambient_light", Vector3());
		}

	/*
	//TONE MAPPING
	shader = Shader::Get("tonemapping");
	shader->enable();

	shader->setUniform("u_texture", illumination_fbo->color_textures[0]);
	shader->setUniform("u_scale", u_scale);
	shader->setUniform("u_average_lum", u_average_lum);
	shader->setUniform("u_lumwhite2", u_lumwhite2);
	shader->setUniform("u_igamma", u_igamma);
	quad->render(GL_TRIANGLES);
	*/

	illumination_fbo->unbind();

	glDisable(GL_BLEND);
	illumination_fbo->color_textures[0]->toViewport();

	if (show_gbuffers)
	{
		glDisable(GL_BLEND);
		glViewport(0, height * 0.5, width * 0.5, height * 0.5);
		gbuffers_fbo->color_textures[0]->toViewport();
		glViewport(width * 0.5, height * 0.5, width * 0.5, height * 0.5);
		gbuffers_fbo->color_textures[1]->toViewport();

		glViewport(0, 0, width * 0.5, height * 0.5);
		gbuffers_fbo->color_textures[2]->toViewport();
		glViewport(width * 0.5, 0, width * 0.5, height * 0.5);

		Shader* shader = Shader::getDefaultShader("depth");
		shader->enable();
		shader->setUniform("u_camera_nearfar", Vector2(camera->near_plane, camera->far_plane));
		gbuffers_fbo->depth_texture->toViewport(shader);
		glViewport(0, 0, width, height);
	}

	if (show_ssao)
	{
		glDisable(GL_BLEND);
		ssao_fbo->color_textures[0]->toViewport();
	}
}

void Renderer::DeferredUniforms(Shader* shader, Scene* scene, Camera* camera, int& width, int& height)
{
	shader->setUniform("u_gb0_texture", gbuffers_fbo->color_textures[0], 0);
	shader->setUniform("u_gb1_texture", gbuffers_fbo->color_textures[1], 1);
	shader->setUniform("u_gb2_texture", gbuffers_fbo->color_textures[2], 2);
	shader->setUniform("u_depth_texture", gbuffers_fbo->depth_texture, 3);
}


void GTR::Renderer::showShadowmap(LightEntity* light)
{
	if (!light->shadowmap)
		return;

	//Use special shader
	Shader* shader = Shader::getDefaultShader("depth");
	shader->enable();
	if (light->light_type == eLightType::DIRECTIONAL)
		shader->setUniform("u_camera_nearfar", Vector2(0, 1));
	else
		shader->setUniform("u_camera_nearfar", Vector2(light->light_camera->near_plane, light->light_camera->far_plane));
	light->shadowmap->toViewport(shader);
}

void Renderer::generateShadowmap(LightEntity* light)
{
	if (light->light_type != eLightType::SPOT && light->light_type != eLightType::DIRECTIONAL)
		return;

	if (!light->cast_shadows)
	{
		if (light->fbo)
		{
			delete light->fbo;
			light->fbo = NULL;
			light->shadowmap = NULL;
		}
		return;
	}
	if (!light->fbo)
	{
		light->fbo = new FBO();
		light->fbo->setDepthOnly(1024, 1024);
		light->shadowmap = light->fbo->depth_texture;
	}

	if (!light->light_camera)
		light->light_camera = new Camera();

	light->fbo->bind();

	Camera* light_camera = light->light_camera;
	Camera* view_camera = Camera::current;

	if (light->light_type == eLightType::SPOT)
	{
		light_camera->setPerspective(light->cone_angle * 2, 1.0, 0.1, light->max_dist);
		light_camera->lookAt(light->model.getTranslation(), light->model * Vector3(0, 0, 1), light->model.rotateVector(Vector3(0, 1, 0)));
	}
	else if (light->light_type == eLightType::DIRECTIONAL)
	{
		Vector3 front = light->model.rotateVector(Vector3(0, 0, 1));
		light_camera->setOrthographic(light->area_size * 0.5, -light->area_size * 0.5, -light->area_size * 0.5, light->area_size * 0.5, 0.1, light->max_dist);
		light_camera->lookAt(view_camera->eye - front * light->max_dist * 0.5, view_camera->eye, light->model.rotateVector(Vector3(0, 1, 0)));
	}
	else 
		return;
	
	light_camera->enable();

	glClear(GL_DEPTH_BUFFER_BIT);

	for (int i = 0; i < render_calls.size(); ++i)
	{
		RenderCall& rc = render_calls[i];
		if (rc.material->alpha_mode == eAlphaMode::BLEND)
			continue;
		if (light_camera->testBoxInFrustum(rc.world_bounding.center, rc.world_bounding.halfsize))
			renderFlatMesh(rc.model, rc.mesh, rc.material, light_camera);
	}

	light->fbo->unbind();
	view_camera->enable();
}

//renders all the prefab
void Renderer::renderPrefab(const Matrix44& model, GTR::Prefab* prefab, Camera* camera)
{
	assert(prefab && "PREFAB IS NULL");
	//assign the model to the root node
	renderNode(model, &prefab->root, camera);
}

//renders a node of the prefab and its children
void Renderer::renderNode(const Matrix44& prefab_model, GTR::Node* node, Camera* camera)
{
	if (!node->visible)
		return;

	//compute global matrix
	Matrix44 node_model = node->getGlobalMatrix(true) * prefab_model;

	//does this node have a mesh? then we must render it
	if (node->mesh && node->material)
	{
		//compute the bounding box of the object in world space (by using the mesh bounding box transformed to world space)
		BoundingBox world_bounding = transformBoundingBox(node_model,node->mesh->box);
		
		//if bounding box is inside the camera frustum then the object is probably visible
		if (camera->testBoxInFrustum(world_bounding.center, world_bounding.halfsize) )
		{
			//render node mesh
			RenderCall rc;
			Vector3 nodepos = node_model.getTranslation();
			rc.material = node->material;
			rc.model = node_model;
			rc.mesh = node->mesh;
			rc.distance_to_camera = nodepos.distance(camera->eye);
			rc.world_bounding = world_bounding;
			render_calls.push_back(rc);
		}
	}

	//iterate recursively with children
	for (int i = 0; i < node->children.size(); ++i)
		renderNode(prefab_model, node->children[i], camera);
}

void Renderer::renderMeshWithMaterialToGBuffers(const Matrix44 model, Mesh* mesh, GTR::Material* material, Camera* camera)
{
	//in case there is nothing to do
	if (!mesh || !mesh->getNumVertices() || !material)
		return;
	assert(glGetError() == GL_NO_ERROR);

	if (material->alpha_mode == eAlphaMode::BLEND)
		return;

	//define locals to simplify coding
	Shader* shader = NULL;
	GTR::Scene* scene = GTR::Scene::instance;

	Texture* texture = material->color_texture.texture;
	Texture* emissive_texture = material->emissive_texture.texture;
	Texture* roughness_texture = material->metallic_roughness_texture.texture;

	if (texture == NULL)
		texture = Texture::getWhiteTexture(); //a 1x1 white texture
	if (emissive_texture == NULL)
		emissive_texture = Texture::getBlackTexture(); //a 1x1 white texture
	if (roughness_texture == NULL)
		roughness_texture = Texture::getWhiteTexture();

	//select if render both sides of the triangles
	if (material->two_sided)
		glDisable(GL_CULL_FACE);
	else
		glEnable(GL_CULL_FACE);
	assert(glGetError() == GL_NO_ERROR);

	shader = Shader::Get("gbuffers");

	assert(glGetError() == GL_NO_ERROR);

	//no shader? then nothing to render
	if (!shader)
		return;
	shader->enable();

	//upload uniforms
	shader->setUniform("u_viewprojection", camera->viewprojection_matrix);
	shader->setUniform("u_camera_position", camera->eye);
	shader->setUniform("u_model", model);
	float t = getTime();
	shader->setUniform("u_time", t);

	shader->setUniform("u_color", material->color);
	if (texture)
		shader->setUniform("u_texture", texture, 0);

	shader->setUniform("u_emissive", material->emissive_factor);
	if (emissive_texture)
		shader->setUniform("u_emissive_texture", emissive_texture, 1);

	shader->setUniform("u_roughness", material->roughness_factor);
	shader->setUniform("u_metallic", material->metallic_factor);
	if (roughness_texture)
		shader->setUniform("u_roughness_texture", roughness_texture, 2);

	//this is used to say which is the alpha threshold to what we should not paint a pixel on the screen (to cut polygons according to texture alpha)
	shader->setUniform("u_alpha_cutoff", material->alpha_mode == GTR::eAlphaMode::MASK ? material->alpha_cutoff : 0);

	if (light_mode == SINGLE)
		renderSinglePass(shader, mesh);
	else if (light_mode == MULTI)
		renderMultiPass(shader, mesh, material);
	else
		mesh->render(GL_TRIANGLES);

	//disable shader
	shader->disable();

	glDisable(GL_BLEND);
	glDepthFunc(GL_LESS);
}

void Renderer::renderFlatMesh(const Matrix44 model, Mesh* mesh, GTR::Material* material, Camera* camera)
{
	//in case there is nothing to do
	if (!mesh || !mesh->getNumVertices() || !material)
		return;
	assert(glGetError() == GL_NO_ERROR);

	//define locals to simplify coding
	Shader* shader = NULL;
	GTR::Scene* scene = GTR::Scene::instance;

	//select if render both sides of the triangles
	if (material->two_sided)
		glDisable(GL_CULL_FACE);
	else
		glEnable(GL_CULL_FACE);
	assert(glGetError() == GL_NO_ERROR);

	//chose a shader
	shader = Shader::Get("shadowmap");

	assert(glGetError() == GL_NO_ERROR);

	//no shader? then nothing to render
	if (!shader)
		return;
	shader->enable();

	//upload uniforms
	shader->setUniform("u_viewprojection", camera->viewprojection_matrix);
	shader->setUniform("u_model", model);

	//this is used to say which is the alpha threshold to what we should not paint a pixel on the screen (to cut polygons according to texture alpha)
	shader->setUniform("u_alpha_cutoff", material->alpha_mode == GTR::eAlphaMode::MASK ? material->alpha_cutoff : 0);

	glDepthFunc(GL_LESS);
	glDisable(GL_BLEND);

	//do the draw call that renders the mesh into the screen
	mesh->render(GL_TRIANGLES);

	//disable shader
	shader->disable();
}

//renders a mesh given its transform and material
void Renderer::renderMeshWithMaterialandLighting(const Matrix44 model, Mesh* mesh, GTR::Material* material, Camera* camera)
{
	//in case there is nothing to do
	if (!mesh || !mesh->getNumVertices() || !material )
		return;
    assert(glGetError() == GL_NO_ERROR);

	//define locals to simplify coding
	Shader* shader = NULL;
	GTR::Scene* scene = GTR::Scene::instance;

	int num_lights = lights.size();

	Texture* texture = material->color_texture.texture;
	Texture* emissive_texture = material->emissive_texture.texture;
	Texture* roughness_texture = material->metallic_roughness_texture.texture;

	if (texture == NULL)
		texture = Texture::getWhiteTexture(); //a 1x1 white texture
	if (emissive_texture == NULL)
		emissive_texture = Texture::getBlackTexture(); //a 1x1 white texture
	if (roughness_texture == NULL)
		roughness_texture = Texture::getWhiteTexture();

	//select if render both sides of the triangles
	if(material->two_sided)
		glDisable(GL_CULL_FACE);
	else
		glEnable(GL_CULL_FACE);
    assert(glGetError() == GL_NO_ERROR);

	//Change light_mode
	if (light_mode == SINGLE)
		shader = Shader::Get("singlelight");
	if (light_mode == MULTI)
		shader = Shader::Get("multilight");
 
    assert(glGetError() == GL_NO_ERROR);

	//no shader? then nothing to render
	if (!shader)
		return;
	shader->enable();

	//upload uniforms
	shader->setUniform("u_viewprojection", camera->viewprojection_matrix);
	shader->setUniform("u_camera_position", camera->eye);
	shader->setUniform("u_model", model );
	float t = getTime();
	shader->setUniform("u_time", t );

	shader->setUniform("u_color", material->color);
	if (texture)
		shader->setUniform("u_texture", texture, 0);

	shader->setUniform("u_emissive", material->emissive_factor);
	if (emissive_texture)
		shader->setUniform("u_emissive_texture", emissive_texture, 1);

	shader->setUniform("u_roughness", material->roughness_factor);
	shader->setUniform("u_metallic", material->metallic_factor);
	if (roughness_texture)
		shader->setUniform("u_roughness_texture", roughness_texture, 2);

	//this is used to say which is the alpha threshold to what we should not paint a pixel on the screen (to cut polygons according to texture alpha)
	shader->setUniform("u_alpha_cutoff", material->alpha_mode == GTR::eAlphaMode::MASK ? material->alpha_cutoff : 0);
	shader->setUniform("u_ambient_light", scene->ambient_light);

	if (light_mode == SINGLE)
		renderSinglePass(shader, mesh);
	else if (light_mode == MULTI)
		renderMultiPass(shader, mesh, material);
	else
		mesh->render(GL_TRIANGLES);

	//disable shader
	shader->disable();

	//set the render state as it was before to avoid problems with future renders
	glDisable(GL_BLEND);
	glDepthFunc(GL_LESS);
}

void Renderer::uploadLightToShader(GTR::LightEntity* light, Shader* shader)
{
	shader->setUniform("u_light_type", (int)light->light_type);

	shader->setUniform("u_light_color", light->color); //* light->intensity
	shader->setUniform("u_light_position", light->model * Vector3());
	shader->setUniform("u_light_max_dist", light->max_dist);

	shader->setUniform("u_light_cone", Vector3(light->cone_angle, light->cone_exp, cos(light->cone_angle * DEG2RAD)));
	//shader->setUniform("u_light_front", light->model.rotateVector(Vector3(0, 0, -1)));

	if (light->shadowmap && light->cast_shadows)
	{
		shader->setUniform("u_light_cast_shadows", 1);
		shader->setTexture("u_light_shadowmap", light->shadowmap, 8);
		shader->setUniform("u_shadow_viewproj", light->light_camera->viewprojection_matrix);
		shader->setUniform("u_light_shadowbias", light->shadow_bias);
	}
	else
		shader->setUniform("u_light_cast_shadows", 0);
}

void Renderer::renderMultiPass(Shader* shader, Mesh* mesh, Material* material)
{	
	glDepthFunc(GL_LEQUAL); 
	glBlendFunc(GL_SRC_ALPHA, GL_ONE);

	if (!lights.size())
	{
		shader->setUniform("u_light_color", Vector3());
		mesh->render(GL_TRIANGLES);
	}
	else
	{
		for (int i = 0; i < lights.size(); ++i)
		{
			
			if (i != 0)
			{
				if (material)
				{
					if (material->alpha_mode == GTR::eAlphaMode::BLEND)
						glBlendFunc(GL_SRC_ALPHA, GL_ONE);
					else
						glEnable(GL_BLEND);
				}
				shader->setVector3("u_ambient_light", Vector3(0, 0, 0));

				if (pipeline == DEFERRED)
				{
					shader->setUniform("u_emissive", false);
					glEnable(GL_BLEND);
					glBlendFunc(GL_ONE, GL_ONE);
				}
				else
					shader->setUniform("u_emissive", Vector3());
			}
			
			if (i == 0)
			{
				if (material->alpha_mode == GTR::eAlphaMode::BLEND)
				{
					glEnable(GL_BLEND);
					glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
				}
				else
					glDisable(GL_BLEND);
			}
			else
			{
				glBlendFunc(GL_SRC_ALPHA, GL_ONE);
				glEnable(GL_BLEND);
			}

			LightEntity* light = lights[i];
			uploadLightToShader(light, shader);

			if (light->light_type  == eLightType::SPOT)
			{
				float cos_angle = cos(lights[i]->cone_angle * DEG2RAD);
				shader->setUniform("u_light_cutoff", cos_angle);
				shader->setUniform("u_light_cone_exp", Vector3(light->cone_angle, light->cone_exp, cos(light->cone_angle * DEG2RAD)));
				shader->setVector3("u_light_direction", light->model.rotateVector(Vector3(0, 0, 1)));
			}
			if (light->light_type == eLightType::DIRECTIONAL) {
				shader->setVector3("u_light_direction", light->model.rotateVector(Vector3(0, 0, 1)));
			}

			//Rest of uniforms
			shader->setUniform("u_light_intensity", light->intensity);
			shader->setUniform("u_shadows", light->cast_shadows);

			mesh->render(GL_TRIANGLES);

			shader->setUniform("u_ambient_light", Vector3());

		}
	}
}

void Renderer::renderSinglePass(Shader* shader, Mesh* mesh)
{

	Vector3 light_position[max_lights];
	Vector3 light_color[max_lights];
	float light_max_distances[max_lights];
	int light_type[max_lights];
	float light_intensity[max_lights];
	float light_cutoff[max_lights];
	float light_cone_exp[max_lights];
	Vector3 light_direction[max_lights];

	for (int i = 0; i < lights.size(); ++i) {

		LightEntity* light = lights[i];

		light_position[i] = light->model * Vector3();
		light_color[i] = light->color;
		light_max_distances[i] = light->max_dist;
		light_type[i] = (int)light->light_type;
		light_intensity[i] = light->intensity;
		light_cutoff[i] = cos(light->cone_angle * DEG2RAD);
		light_cone_exp[i] = light->cone_exp;
		light_direction[i] = light->model.frontVector();
	}

	shader->setUniform3Array("u_light_position", (float*)&light_position, max_lights);
	shader->setUniform3Array("u_light_color", (float*)&light_color, max_lights);
	shader->setUniform1Array("u_light_max_dist", (float*)&light_max_distances, max_lights);
	shader->setUniform1Array("u_light_type", (int*)&light_type, max_lights);
	shader->setUniform1Array("u_light_intensity", (float*)&light_intensity, max_lights);
	shader->setUniform1Array("u_light_cutoff", (float*)&light_cutoff, max_lights);
	shader->setUniform1Array("u_light_cone_exp", (float*)&light_cone_exp, max_lights);
	shader->setUniform1("u_num_lights", (int)lights.size());
	shader->setUniform3Array("u_light_direction", (float*)&light_direction, max_lights);


	//do the draw call that renders the mesh into the screen
	mesh->render(GL_TRIANGLES);
}

std::vector<Vector3> GTR::generateSpherePoints(int num, float radius, bool hemi)
{
	std::vector<Vector3> points;
	points.resize(num);
	for (int i = 0; i < num; i += 3)
	{
		Vector3& p = points[i];
		float u = random();
		float v = random();
		float theta = u * 2.0 * PI;
		float phi = acos(2.0 * v - 1.0);
		float r = cbrt(random() * 0.9 + 0.1) * radius;
		float sinTheta = sin(theta);
		float cosTheta = cos(theta);
		float sinPhi = sin(phi);
		float cosPhi = cos(phi);
		p.x = r * sinPhi * cosTheta;
		p.y = r * sinPhi * sinTheta;
		p.z = r * cosPhi;
		if (hemi && p.z < 0)
			p.z *= -1.0;
	}
	return points;
}

Texture* GTR::CubemapFromHDRE(const char* filename)
{
	HDRE* hdre = HDRE::Get(filename);
	if (!hdre)
		return NULL;

	Texture* texture = new Texture();
	if (hdre->getFacesf(0))
	{
		texture->createCubemap(hdre->width, hdre->height, (Uint8**)hdre->getFacesf(0),
			hdre->header.numChannels == 3 ? GL_RGB : GL_RGBA, GL_FLOAT);
		for (int i = 1; i < hdre->levels; ++i)
			texture->uploadCubemap(texture->format, texture->type, false,
				(Uint8**)hdre->getFacesf(i), GL_RGBA32F, i);
	}
	else
		if (hdre->getFacesh(0))
		{
			texture->createCubemap(hdre->width, hdre->height, (Uint8**)hdre->getFacesh(0),
				hdre->header.numChannels == 3 ? GL_RGB : GL_RGBA, GL_HALF_FLOAT);
			for (int i = 1; i < hdre->levels; ++i)
				texture->uploadCubemap(texture->format, texture->type, false,
					(Uint8**)hdre->getFacesh(i), GL_RGBA16F, i);
		}
	return texture;
}