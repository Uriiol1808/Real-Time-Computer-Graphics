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
#include "sphericalharmonics.h"

using namespace GTR;

Renderer::Renderer()
{
	light_mode = eLightMode::MULTI;
	pipeline = ePipeline::DEFERRED;

	render_shadowmaps = false;

	//GBUFFERS
	gbuffers_fbo = NULL;
	show_gbuffers = false;
	illumination_fbo = NULL;
	
	//SSAO
	ssao_fbo = NULL;
	random_points = generateSpherePoints(64, 1, false);
	show_ssao = false;
	ssao_plus = false;

	//HDR - TONE MAPPING (Random numbers)
	u_scale = 1.0;
	u_average_lum = 2.5;
	u_lumwhite2 = 10.0;
	u_igamma = 2.2;
	show_hdr = false;

	//PROBE
	irr_fbo = NULL;
	probes_texture = NULL;
	show_probes_texture = false;

	//SKYBOX - REFLECTIONS
	skybox = CubemapFromHDRE("data/night.hdre");
	reflection_fbo = new FBO();
	reflection_fbo->create(Application::instance->window_width, Application::instance->window_height);
	is_rendering_reflections = false;
	reflection_probe_fbo = new FBO();
	probe = NULL;

	//VOLUMETRIC
	volumetric_fbo = NULL;
	direct_light = NULL;

	//DECALS
	decals_fbo = NULL;
	cube.createCube(Vector3(1.0, 1.0, 1.0));

	//POSTFX
	postFX_textureA = NULL;
	postFX_textureB = NULL;
	postFX_textureC = NULL;
	postFX_textureD = NULL;
	//Grayscale
	saturation = 1.0f;
	contrast = 1.0f;
	vigneting = 0.0f;
	//Blur
	blur = 0.0f;
	mix_factor = 1.0f;
	threshold = 0.9f;
	//Bloom/Glow
	bloom_threshold = 1.0f;
	bloom_soft_threshold = 0.5f;

	//DoF
	minDistance = 1.0f;
	maxDistance = 3.0f;
	//FXAA

	//LUT

	//Lens Distortion
	distortion = 0.0f;
	//Chromatic Aberration
	chroma = 0.0f;
	//Grain
	noise_amount = 0.0f;
}

void Renderer::renderScene(GTR::Scene* scene, Camera* camera)
{
	camera->enable();
	renderSceneForward(scene, camera);

	renderReflectionProbes(scene, camera);
}

void Renderer::renderSceneForward(GTR::Scene* scene, Camera* camera)
{
	lights.clear();
	render_calls.clear();
	decals.clear();

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
			if (light->light_type == eLightType::DIRECTIONAL)
				direct_light = light;
		}

		//is a decal!
		if (ent->entity_type == DECAL)
		{
			decals.push_back((DecalEntity*)ent);
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
	
	if (pipeline == FORWARD) renderForward(camera, scene);
	else if (pipeline == DEFERRED) renderDeferred(camera, scene);

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

	if (probes_texture && show_probes_texture)
		probes_texture->toViewport();
}

void GTR::Renderer::renderForward(Camera* camera, GTR::Scene* scene)
{
	//set the clear color (the background color)
	glClearColor(scene->background_color.x, scene->background_color.y, scene->background_color.z, 1.0);

	// Clear the color and the depth buffer
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	checkGLErrors();

	renderSkybox(camera);

	for (int i = 0; i < render_calls.size(); ++i)
	{
		RenderCall& rc = render_calls[i];
		if (camera->testBoxInFrustum(rc.world_bounding.center, rc.world_bounding.halfsize))
			renderMeshWithMaterialandLighting(rc.model, rc.mesh, rc.material, camera);
	}

	for (int i = 0; i < probes.size(); ++i)
	{
		renderProbe(probes[i].pos, 2, probes[i].sh.coeffs[0].v);
	}
}

void GTR::Renderer::renderDeferred(Camera* camera, GTR::Scene* scene)
{
	int width = Application::instance->window_width;
	int height = Application::instance->window_height;

	Mesh* quad = Mesh::getQuad(); //2 triangulos que forman un cuadraro en clip space (-1,1 a 1,1)
	Shader* shader = NULL;

	Matrix44 inv_vp = camera->viewprojection_matrix;
	inv_vp.inverse();

	//create and FBO
	if (!gbuffers_fbo){

		gbuffers_fbo = new FBO();
		illumination_fbo = new FBO();
		ssao_fbo = new FBO();
		decals_fbo = new FBO();

		//create 3 textures of 4 components
		gbuffers_fbo->create(width, height,
			3, 				
			GL_RGBA, 		
			GL_UNSIGNED_BYTE,
			true);			

		//create 1 textures of 3 components
		illumination_fbo->create(width, height,
			1, 					
			GL_RGB, 			
			GL_FLOAT,	
			true);				
		
		ssao_fbo->create(width, height,
			1,
			GL_LUMINANCE,
			GL_UNSIGNED_BYTE,
			false);

		decals_fbo->create(width, height,
			3,
			GL_RGBA,
			GL_UNSIGNED_BYTE,
			true);

		postFX_textureA = new Texture(width, height, GL_RGB, GL_FLOAT, false);
		postFX_textureB = new Texture(width, height, GL_RGB, GL_FLOAT, false);
		postFX_textureC = new Texture(width, height, GL_RGB, GL_FLOAT, false);
		postFX_textureD = new Texture(width, height, GL_RGB, GL_FLOAT, false);
	}

	//------GBUFFERS-------
	gbuffers_fbo->bind();

	//set the clear color (the background color)
	glClearColor(scene->background_color.x, scene->background_color.y, scene->background_color.z, 1.0);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	checkGLErrors();

	//Renderizar cada objecto con un GBUffer shader
	for (int i = 0; i < render_calls.size(); ++i)
	{
		RenderCall& rc = render_calls[i];
		if (camera->testBoxInFrustum(rc.world_bounding.center, rc.world_bounding.halfsize))
			renderMeshWithMaterialToGBuffers(rc.model, rc.mesh, rc.material, camera);
	}

	gbuffers_fbo->unbind();

	//-------DECALS------
	gbuffers_fbo->color_textures[0]->copyTo(decals_fbo->color_textures[0]);
	gbuffers_fbo->color_textures[1]->copyTo(decals_fbo->color_textures[1]);
	gbuffers_fbo->color_textures[2]->copyTo(decals_fbo->color_textures[2]);

	decals_fbo->bind();
	gbuffers_fbo->depth_texture->copyTo(NULL);
	decals_fbo->unbind();

	if (decals.size())
	{
		gbuffers_fbo->bind();
		shader = Shader::Get("decal");
		shader->enable();

		shader->setUniform("u_gb0_texture", decals_fbo->color_textures[0], 0);
		shader->setUniform("u_gb1_texture", decals_fbo->color_textures[1], 1);
		shader->setUniform("u_gb2_texture", decals_fbo->color_textures[2], 2);
		shader->setUniform("u_depth_texture", decals_fbo->depth_texture, 3);

		shader->setUniform("u_camera_position", camera->eye);
		shader->setUniform("u_viewprojection", camera->viewprojection_matrix);
		shader->setUniform("u_inverse_viewprojection", inv_vp);
		shader->setUniform("u_iRes", Vector2(1.0 / (float)width, 1.0 / (float)height));

		glEnable(GL_BLEND);
		glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

		for (int i = 0; i < decals.size(); i++)
		{
			DecalEntity* decal = decals[i];

			Texture* decal_texture = Texture::Get(decal->texture.c_str());
			if (!decal_texture) 
				continue;
			shader->setUniform("u_decal_texture", decal_texture, 4);
			shader->setUniform("u_model", decal->model);

			Matrix44 imodel = decal->model;
			imodel.inverse(); 
			shader->setUniform("u_imodel", imodel); //World space to local
			cube.render(GL_TRIANGLES);
		}
		glDisable(GL_BLEND);

		gbuffers_fbo->unbind();
	}


	//-------SSAO-------
	ssao_fbo->bind();

	glDisable(GL_DEPTH_TEST);
	glDisable(GL_BLEND);

	if (ssao_plus) {shader = Shader::Get("ssao_plus");}
	else { shader = Shader::Get("ssao"); }
	
	shader->enable();

	shader->setUniform("u_gb1_texture", gbuffers_fbo->color_textures[1], 1);
	shader->setUniform("u_depth_texture", gbuffers_fbo->depth_texture, 3);
	shader->setUniform("u_viewprojection", camera->viewprojection_matrix);
	shader->setUniform("u_inverse_viewprojection", inv_vp);
	shader->setUniform("u_iRes", Vector2(1.0 / (float)width, 1.0 / (float)height));
	shader->setUniform3Array("u_points", (float*)&random_points[0], random_points.size());

	quad->render(GL_TRIANGLES);

	ssao_fbo->unbind();
	
	//-------ILLUMINATION-------
	illumination_fbo->bind();

	gbuffers_fbo->depth_texture->copyTo(NULL);
	
	glDisable(GL_DEPTH_TEST);
	glClearColor(scene->background_color.x, scene->background_color.y, scene->background_color.z, 1.0);
	glClear(GL_COLOR_BUFFER_BIT);

	renderSkybox(camera);
	//we need a fullscreen quad
	shader = Shader::Get("deferred");
	shader->enable();

	GbuffersShader(shader, scene, camera); //gb0, gb1, gb2, depth
	shader->setUniform("u_ssao_texture", ssao_fbo->color_textures[0], 5);
	//shader->setUniform("u_irr_texture", probes_texture, 6);

	shader->setUniform("u_camera_position", camera->eye);
	shader->setUniform("u_ambient_light", scene->ambient_light);
	shader->setUniform("u_inverse_viewprojection", inv_vp);
	shader->setUniform("u_iRes", Vector2(1.0 / (float)width, 1.0 / (float)height));

	if (!lights.size())
	{
		shader->setUniform("u_light_color", Vector3());
		quad->render(GL_TRIANGLES);
	}
	else
		for (int i = 0; i < lights.size(); ++i)
		{
			LightEntity* light = lights[3]; //3 direccional porque sino solo coge la primera del json :/

			if (i == 0)
				glDisable(GL_BLEND);
			else {
				glBlendFunc(GL_SRC_ALPHA, GL_ONE);
				glEnable(GL_BLEND);
			}

			uploadLightToShader(light, shader);

			//do the draw call that renders the mesh into the screen
			quad->render(GL_TRIANGLES);

			shader->setUniform("u_ambient_light", Vector3());
		}
	glDisable(GL_CULL_FACE);

	//-------IRRADIANCE-------
	if (probes_texture) {
		shader = Shader::Get("irradiance");
		shader->enable();

		GbuffersShader(shader, scene, camera);
		shader->setUniform("u_inverse_viewprojection", inv_vp);
		shader->setUniform("u_iRes", Vector2(1.0 / (float)width, 1.0 / (float)height));

		shader->setUniform("u_ssao_texture", ssao_fbo->color_textures[0], 5);
		shader->setUniform("u_irr_texture", probes_texture, 6);
		shader->setUniform("u_irr_start", start_irr);
		shader->setUniform("u_irr_end", end_irr);
		shader->setUniform("u_irr_dim", dim_irr);

		shader->setUniform("u_irr_normal_distance", 0.1f);
		shader->setUniform("u_num_probes", probes_texture->height);
		shader->setUniform("u_irr_delta", end_irr - start_irr);

		glEnable(GL_BLEND);
		glBlendFunc(GL_SRC_ALPHA, GL_ONE);
		
		quad->render(GL_TRIANGLES);
	}

	//-------ALPHA-------
	//gbuffers_fbo->depth_texture->copyTo(NULL); //Depth buffer
	
	glEnable(GL_DEPTH_TEST);
	glEnable(GL_BLEND);

	for (int i = 0; i < render_calls.size(); i++) {
		RenderCall& rc = render_calls[i];
		if (rc.material->alpha_mode == eAlphaMode::BLEND)
			if (camera->testBoxInFrustum(rc.world_bounding.center, rc.world_bounding.halfsize))
				renderMeshWithMaterialToGBuffers(rc.model, rc.mesh, rc.material, camera);
	}

	illumination_fbo->unbind();
	glDisable(GL_BLEND);

	applyFX(illumination_fbo->color_textures[0], gbuffers_fbo->depth_texture, camera);

	//-------HDR - TONE MAPPING-------	
	if (show_hdr)
	{
		shader = Shader::Get("tonemapping");
		shader->enable();

		shader->setUniform("u_scale", u_scale);
		shader->setUniform("u_average_lum", u_average_lum);
		shader->setUniform("u_lumwhite2", u_lumwhite2);
		shader->setUniform("u_igamma", u_igamma);

		illumination_fbo->color_textures[0]->toViewport(shader);
	}
	
	//-------VOLUMETRIC-------
	if (direct_light)
	{
		if (!volumetric_fbo)
		{
			volumetric_fbo = new FBO();
			volumetric_fbo->create(width/2, height/2, 1, GL_RGBA);
		}

		volumetric_fbo->bind();

		shader = Shader::Get("volumetric");
		shader->enable();
		shader->setUniform("u_camera_position", camera->eye);
		shader->setUniform("u_depth_texture", gbuffers_fbo->depth_texture, 3);
		shader->setUniform("u_inverse_viewprojection", inv_vp);
		shader->setUniform("u_air_density", scene->air_density * 0.001f);
		shader->setUniform("u_iRes", Vector2(1.0 / (float)volumetric_fbo->color_textures[0]->width, 1.0 / (float)volumetric_fbo->color_textures[0]->height));
		uploadLightToShader(direct_light, shader);

		quad->render(GL_TRIANGLES);
		volumetric_fbo->unbind();
		glEnable(GL_BLEND);
		//glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
		glBlendFunc(GL_SRC_ALPHA, GL_ONE);
		volumetric_fbo->color_textures[0]->toViewport();
		glDisable(GL_BLEND);
	}

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

void GTR::Renderer::applyFX(Texture* color_texture, Texture* depth_texture, Camera* camera)
{
	Texture* current_texture = color_texture;
	Shader* fxshader = NULL;
	FBO* fbo = NULL;
	int width = Application::instance->window_width;
	int height = Application::instance->window_height;

	Matrix44 inv_vp = camera->viewprojection_matrix;
	inv_vp.inverse();

	//Blur 
	for (int i = 0; i < 8; ++i)
	{
		fbo = Texture::getGlobalFBO(postFX_textureA);
		fbo->bind();
		fxshader = Shader::Get("blur");
		fxshader->enable();
		fxshader->setUniform("u_intensity", 1.0f);
		fxshader->setUniform("u_offset", vec2(pow(1.0f, i) / current_texture->width, 0.0) * blur); //Horizontal
		current_texture->toViewport(fxshader);
		fbo->unbind();

		fbo = Texture::getGlobalFBO(postFX_textureB);
		fbo->bind();
		fxshader = Shader::Get("blur");
		fxshader->enable();
		fxshader->setUniform("u_intensity", 1.0f);
		fxshader->setUniform("u_offset", vec2(0.0f, pow(1.0f, i) / current_texture->height) * blur); //Vertical
		postFX_textureA->toViewport(fxshader);
		fbo->unbind();
		current_texture = postFX_textureB;
	}
	/*
	//Bloom
	fbo = Texture::getGlobalFBO(postFX_textureA);
	fbo->bind();
	fxshader = Shader::Get("bloom");
	fxshader->enable();
	fxshader->setUniform("threshold", bloom_threshold);
	fxshader->setUniform("soft_threshold", bloom_soft_threshold);
	current_texture->toViewport(fxshader);
	fbo->unbind();
	current_texture = postFX_textureA;
	std::swap(postFX_textureA, postFX_textureB);
	*/

	//Depth of Field
	fbo = Texture::getGlobalFBO(postFX_textureA);
	fbo->bind();
	fxshader = Shader::Get("dof");
	fxshader->enable();

	fxshader->setUniform("minDistance", minDistance);
	fxshader->setUniform("maxDistance", maxDistance);

	fxshader->setUniform("outOfFocusTexture", postFX_textureB, 1);
	fxshader->setUniform("u_depth_texture", depth_texture, 2);
	fxshader->setUniform("u_inverse_viewprojection", inv_vp);
	fxshader->setUniform("u_iRes", Vector2(1.0 / (float)width, 1.0 / (float)height));

	current_texture->toViewport(fxshader);
	fbo->unbind();
	current_texture = postFX_textureA;
	std::swap(postFX_textureA, postFX_textureB);

	//Motion Blur
	fbo = Texture::getGlobalFBO(postFX_textureA);
	fbo->bind();
	fxshader = Shader::Get("motionblur");
	fxshader->enable();
	fxshader->setUniform("u_depth_texture", depth_texture, 1);
	fxshader->setUniform("u_inverse_viewprojection", inv_vp);
	fxshader->setUniform("u_viewprojection_old", vp_matrix_last);
	current_texture->toViewport(fxshader);
	fbo->unbind();
	current_texture = postFX_textureA;
	std::swap(postFX_textureA, postFX_textureB);

	vp_matrix_last = camera->viewprojection_matrix;

	//Contrast + Saturation + Vigneting
	fbo = Texture::getGlobalFBO(postFX_textureA);
	fbo->bind();
	fxshader = Shader::Get("greyscale");
	fxshader->enable();
	fxshader->setUniform("u_saturation", saturation);
	fxshader->setUniform("u_vigneting", vigneting);
	current_texture->toViewport(fxshader);
	fbo->unbind();
	current_texture = postFX_textureA;
	std::swap(postFX_textureA, postFX_textureB);

	//Bloom 
	fbo = Texture::getGlobalFBO(postFX_textureC); //Grayscale image
	fbo->bind();
	fxshader = Shader::Get("contrast");
	fxshader->enable();
	fxshader->setUniform("u_intensity", contrast);
	current_texture->toViewport(fxshader);
	fbo->unbind();
	current_texture = postFX_textureC;

	fbo = Texture::getGlobalFBO(postFX_textureD); //Threshold
	fbo->bind();
	fxshader = Shader::Get("threshold");
	fxshader->enable();
	fxshader->setUniform("u_threshold", threshold);
	current_texture->toViewport(fxshader);
	fbo->unbind();
	current_texture = postFX_textureD;

	fbo = Texture::getGlobalFBO(postFX_textureA);
	fbo->bind();
	fxshader = Shader::Get("mix");
	fxshader->enable();
	fxshader->setUniform("u_intensity", mix_factor);
	fxshader->setUniform("u_textureB", postFX_textureC, 1);
	current_texture->toViewport(fxshader);
	fbo->unbind();
	current_texture = postFX_textureA;
	std::swap(postFX_textureA, postFX_textureB);

	//Grain
	fbo = Texture::getGlobalFBO(postFX_textureA);
	fbo->bind();
	fxshader = Shader::Get("grain");
	fxshader->enable();
	fxshader->setUniform("amount", float(abs(cos(getTime()))));
	fxshader->setUniform("tDiffuse", postFX_textureB, 1);
	fxshader->setUniform("noise_amount", noise_amount);
	current_texture->toViewport(fxshader);
	fbo->unbind();
	current_texture = postFX_textureA;
	std::swap(postFX_textureA, postFX_textureB);

	//Chromatic Aberration
	fbo = Texture::getGlobalFBO(postFX_textureA);
	fbo->bind();
	fxshader = Shader::Get("chromatic_aberration");
	fxshader->enable();
	fxshader->setUniform("u_amount", chroma);
	fxshader->setUniform("u_iRes", Vector2(1.0 / (float)width, 1.0 / (float)height));
	current_texture->toViewport(fxshader);
	fbo->unbind();
	current_texture = postFX_textureA;
	std::swap(postFX_textureA, postFX_textureB);

	//Lens Distortion 
	fbo = Texture::getGlobalFBO(postFX_textureA);
	fbo->bind();
	fxshader = Shader::Get("lens_distortion");
	fxshader->enable();
	fxshader->setUniform("u_iRes", Vector2(1.0 / (float)width, 1.0 / (float)height));
	fxshader->setUniform("u_resolution", distortion);
	current_texture->toViewport(fxshader);
	fbo->unbind();
	current_texture = postFX_textureA;
	std::swap(postFX_textureA, postFX_textureB);

	current_texture->toViewport();

	/*
	//FXAA NO
	fbo = Texture::getGlobalFBO(postFX_textureA);
	fbo->bind();
	fxshader = Shader::Get("fxaa");
	fxshader->enable();
	fxshader->setUniform("u_viewportSize", Vector2(1.0 / (float)width, 1.0 / (float)height));
	fxshader->setUniform("u_iViewportSize", Vector2(1.0 / (float)width, 1.0 / (float)height));
	fxshader->setTexture("tex", illumination_fbo->color_textures[0], 0);
	current_texture->toViewport(fxshader);
	fbo->unbind();
	current_texture = postFX_textureA;
	std::swap(postFX_textureA, postFX_textureB);
	current_texture->toViewport();
	*/

	//LUT

	/*
	//Lens Distortion SI
	fbo = Texture::getGlobalFBO(postFX_textureA);
	fbo->bind();
	fxshader = Shader::Get("lens_distortion");
	fxshader->enable();
	fxshader->setUniform("u_iRes", Vector2(1.0 / (float)width, 1.0 / (float)height));
	fxshader->setUniform("u_resolution", resolution);
	current_texture->toViewport(fxshader);
	fbo->unbind();
	current_texture = postFX_textureA;
	std::swap(postFX_textureA, postFX_textureB);

	current_texture->toViewport();
	*/



	/*
	
	//TONEMAPPER
	*/
}

void Renderer::GbuffersShader(Shader* shader, Scene* scene, Camera* camera)
{
	shader->setUniform("u_gb0_texture", gbuffers_fbo->color_textures[0], 1);
	shader->setUniform("u_gb1_texture", gbuffers_fbo->color_textures[1], 2);
	shader->setUniform("u_gb2_texture", gbuffers_fbo->color_textures[2], 3);

	shader->setUniform("u_depth_texture", gbuffers_fbo->depth_texture, 4);
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
			rc.material = node->material;
			rc.model = node_model;
			rc.mesh = node->mesh;
			rc.distance_to_camera = camera->eye.distance(node->mesh->box.center);
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
	Texture* normalmap_texture = material->normal_texture.texture;

	if (texture == NULL)
		texture = Texture::getWhiteTexture(); //a 1x1 white texture
	if (emissive_texture == NULL)
		emissive_texture = Texture::getWhiteTexture(); //a 1x1 white texture
	if (roughness_texture == NULL)
		roughness_texture = Texture::getWhiteTexture();
	if (normalmap_texture == NULL)
		normalmap_texture = Texture::getBlackTexture();

	if (material->alpha_mode == GTR::eAlphaMode::BLEND)
	{
		glEnable(GL_BLEND);
		glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	}
	else
		glDisable(GL_BLEND);

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

	if (normalmap_texture)
		shader->setUniform("u_texture_normals", normalmap_texture, 3);

	//this is used to say which is the alpha threshold to what we should not paint a pixel on the screen (to cut polygons according to texture alpha)
	shader->setUniform("u_alpha_cutoff", material->alpha_mode == GTR::eAlphaMode::MASK ? material->alpha_cutoff : 0);

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
	Texture* normalmap_texture = material->normal_texture.texture;

	if (texture == NULL)
		texture = Texture::getWhiteTexture(); //a 1x1 white texture
	if (emissive_texture == NULL)
		emissive_texture = Texture::getBlackTexture(); //a 1x1 white texture
	if (roughness_texture == NULL)
		roughness_texture = Texture::getWhiteTexture();
	if (normalmap_texture == NULL)
		normalmap_texture = Texture::getBlackTexture();

	if (material->alpha_mode == GTR::eAlphaMode::BLEND)
	{
		glEnable(GL_BLEND);
		glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	}
	else
		glDisable(GL_BLEND);

	//select if render both sides of the triangles
	if(material->two_sided)
		glDisable(GL_CULL_FACE);
	else
		glEnable(GL_CULL_FACE);
    assert(glGetError() == GL_NO_ERROR);

	//Change light_mode
	if (light_mode == SINGLE)
		shader = Shader::Get("singlelight");
	else if (light_mode == MULTI)
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
	shader->setUniform("u_ambient_light", scene->ambient_light);
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

	if (normalmap_texture)
		shader->setUniform("u_texture_normals", normalmap_texture, 3);

	//this is used to say which is the alpha threshold to what we should not paint a pixel on the screen (to cut polygons according to texture alpha)
	shader->setUniform("u_alpha_cutoff", material->alpha_mode == GTR::eAlphaMode::MASK ? material->alpha_cutoff : 0);

	Texture* reflection = skybox;
	if (probe && !is_rendering_reflections)
		reflection = probe->texture;
	shader->setUniform("u_skybox_texture", reflection, 8);

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

	shader->setUniform("u_light_cone_exp", Vector3(light->cone_angle, light->cone_exp, cos(light->cone_angle * DEG2RAD)));
	shader->setVector3("u_light_direction", light->model.frontVector());
	shader->setUniform("u_light_intensity", light->intensity);

	if (light->shadowmap && light->cast_shadows)
	{
		shader->setUniform("u_light_cast_shadows", 1);
		shader->setTexture("u_light_shadowmap", light->shadowmap, 8);
		shader->setUniform("u_shadow_viewproj", light->light_camera->viewprojection_matrix);
		shader->setUniform("u_light_shadowbias", light->shadow_bias);
	}
	else
		shader->setUniform("u_light_cast_shadows", 0);

	if (light->light_type == DIRECTIONAL) {

		shader->setVector3("u_light_vector", light->model * Vector3() - light->target);
	}
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

			mesh->render(GL_TRIANGLES);

			shader->setUniform("u_ambient_light", Vector3());
			shader->setUniform("u_emissive", Vector3());
		}
	}
}

void Renderer::renderSinglePass(Shader* shader, Mesh* mesh)
{
	glDepthFunc(GL_LEQUAL);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE);

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
	for (int i = 0; i < num; i += 1)
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

//PROBE IRRADIANCE
void GTR::Renderer::renderProbe(Vector3 pos, float size, float* coeffs)
{ 
	Camera* camera = Camera::current;
	Shader* shader = Shader::Get("probe");
	Mesh* mesh = Mesh::Get("data/meshes/sphere.obj", false);

	glEnable(GL_CULL_FACE);
	glDisable(GL_BLEND);
	glEnable(GL_DEPTH_TEST);

	Matrix44 model;
	model.setTranslation(pos.x, pos.y, pos.z);
	model.scale(size, size, size);

	shader->enable();
	shader->setUniform("u_viewprojection", camera->viewprojection_matrix);
	shader->setUniform("u_camera_position", camera->eye);
	shader->setUniform("u_model", model);
	shader->setUniform3Array("u_coeffs", coeffs, 9);

	mesh->render(GL_TRIANGLES);
}

void GTR::Renderer::captureProbe(sProbe& p, GTR::Scene* scene)
{
	FloatImage images[6]; //here we will store the six views
	Camera cam;

//set the fov to 90 and the aspect to 1
	cam.setPerspective(90, 1, 0.1, 1000);

	if (irr_fbo == NULL) 
	{
		irr_fbo = new FBO();
		irr_fbo->create(64, 64, 1, GL_RGB, GL_FLOAT);
	}

	for (int i = 0; i < 6; ++i) //for every cubemap face
	{
		//compute camera orientation using defined vectors
		Vector3 eye = p.pos;
		Vector3 front = cubemapFaceNormals[i][2];
		Vector3 center = p.pos + front;
		Vector3 up = cubemapFaceNormals[i][1];
		cam.lookAt(eye, center, up);
		cam.enable();

		//render the scene from this point of view
		irr_fbo->bind();
		renderForward(&cam, scene);
		irr_fbo->unbind();

		//read the pixels back and store in a FloatImage
		images[i].fromTexture(irr_fbo->color_textures[0]);
	}

	//compute the coefficients given the six images
	p.sh = computeSH(images);
}

void GTR::Renderer::generateProbes(GTR::Scene* scene)
{
	//define the corners of the axis aligned grid
	//this can be done using the boundings of our scene
	Vector3 start_pos(-300, 5, -300);
	Vector3 end_pos(300, 150, 300);
	Vector3 dim(10, 4, 10);
	Vector3 delta = (end_pos - start_pos);

	start_irr = start_pos;
	end_irr = end_pos;
	dim_irr = dim;
	delta_irr = delta;

	probes.clear();

	//and scale it down according to the subdivisions
	//we substract one to be sure the last probe is at end pos
	delta.x /= (dim.x - 1);
	delta.y /= (dim.y - 1);
	delta.z /= (dim.z - 1);

	for (int z = 0; z < dim.z; ++z) {
		for (int y = 0; y < dim.y; ++y) {
			for (int x = 0; x < dim.x; ++x)
			{
				sProbe p;
				p.local.set(x, y, z);

				//index in the linear array
				p.index = x + y * dim.x + z * dim.x * dim.y;

				//and its position
				p.pos = start_pos + delta * Vector3(x, y, z);
				probes.push_back(p);
			}
		}
	}
	std::cout << std::endl;
	//now compute the coeffs for every probe
	for (int iP = 0; iP < probes.size(); ++iP)
	{
		int probe_index = iP;
		captureProbe(probes[iP], scene);
		std::cout << "Generating Probes: " << iP << "/" << probes.size() << "\r";
	}
	std::cout << "DONE" << std::endl;

	if (probes_texture != NULL)
		delete probes_texture;

	//create the texture to store the probes (do this ONCE!!!)
	probes_texture = new Texture(
		9, //9 coefficients per probe
		probes.size(), //as many rows as probes
		GL_RGB, //3 channels per coefficient
		GL_FLOAT); //they require a high range

	//we must create the color information for the texture. because every SH are 27 floats in the RGB,RGB,... order, we can create an array of SphericalHarmonics and use it as pixels of the texture
	SphericalHarmonics* sh_data = NULL;
	sh_data = new SphericalHarmonics[dim.x * dim.y * dim.z];

	//here we fill the data of the array with our probes in x,y,z order
	for (int i = 0; i < probes.size(); ++i)
		sh_data[i] = probes[i].sh;

	//now upload the data to the GPU as a texture
	probes_texture->upload(GL_RGB, GL_FLOAT, false, (uint8*)sh_data);

	//disable any texture filtering when reading
	probes_texture->bind();
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	probes_texture->unbind();

	//always free memory after allocating it!!!
	delete[] sh_data;
}

//SKYBOX
void GTR::Renderer::renderSkybox(Camera* camera)
{
	Mesh* mesh = Mesh::Get("data/meshes/sphere.obj", false);
	Shader* shader = Shader::Get("skybox");
	shader->enable();

	Matrix44 model;

	glDisable(GL_CULL_FACE);
	glDisable(GL_DEPTH_TEST);

	model.setTranslation(camera->eye.x, camera->eye.y, camera->eye.z);
	model.scale(5, 5, 5);

	shader->setUniform("u_viewprojection", camera->viewprojection_matrix);
	shader->setUniform("u_camera_position", camera->eye);
	shader->setUniform("u_model", model);
	shader->setUniform("u_texture", skybox, 0);

	mesh->render(GL_TRIANGLES);
	shader->disable();

	glEnable(GL_CULL_FACE);
	glEnable(GL_DEPTH_TEST);
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

//REFLECTION
void GTR::Renderer::updateReflectionProbes(Scene* scene)
{
	for (int i = 0; i < scene->entities.size(); ++i)
	{
		BaseEntity* ent = scene->entities[i];
		if (!ent->visible || ent->entity_type != eEntityType::REFLECTION_PROBE)
			continue;
		ReflectionProbeEntity* probe = (ReflectionProbeEntity*)ent;
		if (!probe->texture)
		{
			probe->texture = new Texture();
			probe->texture->createCubemap(256, 256, NULL, GL_RGB, GL_UNSIGNED_INT, false);
		}
		captureReflectionProbe(scene, probe->texture, probe->model.getTranslation());
		this->probe = probe;
	}
}

void GTR::Renderer::renderReflectionProbes(Scene* scene, Camera* camera)
{
	Mesh* mesh = Mesh::Get("data/meshes/sphere.obj", false);
	Shader* shader = Shader::Get("reflection_probe");
	shader->enable();

	shader->setUniform("u_viewprojection", camera->viewprojection_matrix);
	shader->setUniform("u_camera_position", camera->eye);

	glEnable(GL_CULL_FACE);
	glEnable(GL_DEPTH_TEST);
	glEnable(GL_TEXTURE_CUBE_MAP_SEAMLESS);

	for (int i = 0; i < scene->entities.size(); ++i)
	{
		BaseEntity* ent = scene->entities[i];
		if (!ent->visible || ent->entity_type != eEntityType::REFLECTION_PROBE)
			continue;
		ReflectionProbeEntity* probe = (ReflectionProbeEntity*)ent;
		if (!probe->texture)
			continue;

		Matrix44 model = ent->model;
		model.scale(10, 10, 10);
		shader->setUniform("u_model", model);
		shader->setUniform("u_texture", probe->texture, 0);

		mesh->render(GL_TRIANGLES);
	}
	shader->disable();
}

void GTR::Renderer::captureReflectionProbe(GTR::Scene* scene, Texture* texture, Vector3 pos)
{
	for (int i = 0; i < 6; ++i)
	{
		reflection_probe_fbo->setTexture(texture, i);

		Camera camera;
		camera.setPerspective(90, 1, 0.1, 1000);
		Vector3 eye = pos;
		Vector3 center = pos + cubemapFaceNormals[i][2];
		Vector3 up = cubemapFaceNormals[i][1];
		camera.lookAt(eye, center, up);
		camera.enable();

		reflection_probe_fbo->bind();
		is_rendering_reflections = true;
		renderSceneForward(scene, &camera);
		is_rendering_reflections = false;
		reflection_probe_fbo->unbind();

		texture->generateMipmaps();
	}
}