#include "scene.h"
#include "utils.h"

#include "prefab.h"
#include "extra/cJSON.h"
#include "application.h"

GTR::Scene* GTR::Scene::instance = NULL;

GTR::Scene::Scene()
{
	instance = this;
	air_density = 1.0;
}

void GTR::Scene::clear()
{
	for (int i = 0; i < entities.size(); ++i)
	{
		BaseEntity* ent = entities[i];
		delete ent;
	}
	entities.resize(0);
}


void GTR::Scene::addEntity(BaseEntity* entity)
{
	entities.push_back(entity);
	entity->scene = this;
	if(entity->name.size())
		entities_by_name[entity->name] = entity;
}

GTR::BaseEntity* GTR::Scene::getEntityByName(std::string name)
{
	auto it = entities_by_name.find(name);
	if (it == entities_by_name.end())
		return NULL;
	return it->second;
}

bool GTR::Scene::load(const char* filename)
{
	std::string content;

	this->filename = filename;
	std::cout << " + Reading scene JSON: " << filename << "..." << std::endl;

	if (!readFile(filename, content))
	{
		std::cout << "- ERROR: Scene file not found: " << filename << std::endl;
		return false;
	}

	//parse json string 
	cJSON* json = cJSON_Parse(content.c_str());
	if (!json)
	{
		std::cout << "ERROR: Scene JSON has errors: " << filename << std::endl;
		return false;
	}

	//read global properties
	background_color = readJSONVector3(json, "background_color", background_color);
	ambient_light = readJSONVector3(json, "ambient_light", ambient_light );
	main_camera.eye = readJSONVector3(json, "camera_position", main_camera.eye);
	main_camera.center = readJSONVector3(json, "camera_target", main_camera.center);
	main_camera.fov = readJSONNumber(json, "camera_fov", main_camera.fov);

	//entities
	cJSON* entities_json = cJSON_GetObjectItemCaseSensitive(json, "entities");
	cJSON* entity_json;
	cJSON_ArrayForEach(entity_json, entities_json)
	{
		std::string type_str = cJSON_GetObjectItem(entity_json, "type")->valuestring;
		BaseEntity* ent = createEntity(type_str);
		if (!ent)
		{
			std::cout << " - ENTITY TYPE UNKNOWN: " << type_str << std::endl;
			//continue;
			ent = new BaseEntity();
		}

		addEntity(ent);

		if (cJSON_GetObjectItem(entity_json, "name"))
		{
			ent->name = cJSON_GetObjectItem(entity_json, "name")->valuestring;
			stdlog(std::string(" + entity: ") + ent->name);
		}

		//read transform
		if (cJSON_GetObjectItem(entity_json, "position"))
		{
			ent->model.setIdentity();
			Vector3 position = readJSONVector3(entity_json, "position", Vector3());
			ent->model.translate(position.x, position.y, position.z);
		}

		if (cJSON_GetObjectItem(entity_json, "angle"))
		{
			float angle = cJSON_GetObjectItem(entity_json, "angle")->valuedouble;
			ent->model.rotate(angle * DEG2RAD, Vector3(0, 1, 0));
		}

		if (cJSON_GetObjectItem(entity_json, "rotation"))
		{
			Vector4 rotation = readJSONVector4(entity_json, "rotation");
			Quaternion q(rotation.x, rotation.y, rotation.z, rotation.w);
			Matrix44 R;
			q.toMatrix(R);
			ent->model = R * ent->model;
		}

		if (cJSON_GetObjectItem(entity_json, "target"))
		{
			Vector3 target = readJSONVector3(entity_json, "target", Vector3());
			Vector3 front = target - ent->model.getTranslation();
			ent->model.setFrontAndOrthonormalize(front);
		}

		if (cJSON_GetObjectItem(entity_json, "scale"))
		{
			Vector3 scale = readJSONVector3(entity_json, "scale", Vector3(1, 1, 1));
			ent->model.scale(scale.x, scale.y, scale.z);
		}

		ent->configure(entity_json);
	}

	//free memory
	cJSON_Delete(json);

	return true;
}

GTR::BaseEntity* GTR::Scene::createEntity(std::string type)
{
	if (type == "PREFAB")
		return new GTR::PrefabEntity();
	else if (type == "LIGHT")
		return new GTR::LightEntity();
    return NULL;
}

void GTR::BaseEntity::renderInMenu()
{
#ifndef SKIP_IMGUI
	ImGui::Text("Name: %s", name.c_str()); // Edit 3 floats representing a color
	ImGui::Checkbox("Visible", &visible); // Edit 3 floats representing a color
	//Model edit
	ImGuiMatrix44(model, "Model");
#endif
}




GTR::PrefabEntity::PrefabEntity()
{
	entity_type = PREFAB;
	prefab = NULL;
}

void GTR::PrefabEntity::configure(cJSON* json)
{
	if (cJSON_GetObjectItem(json, "filename"))
	{
		filename = cJSON_GetObjectItem(json, "filename")->valuestring;
		prefab = GTR::Prefab::Get( (std::string("data/") + filename).c_str());
	}
}

void GTR::PrefabEntity::renderInMenu()
{
	BaseEntity::renderInMenu();

#ifndef SKIP_IMGUI
	ImGui::Text("filename: %s", filename.c_str()); // Edit 3 floats representing a color
	if (prefab && ImGui::TreeNode(prefab, "Prefab Info"))
	{
		prefab->root.renderInMenu();
		ImGui::TreePop();
	}
#endif
}

GTR::LightEntity::LightEntity()
{
	entity_type = eEntityType::LIGHT;
	color.set(1, 1, 1);
	intensity = 1;
	max_dist = 100;
	cone_angle = 60;
	cone_exp = 60;
	cast_shadows = false;
	shadow_bias = 0.001;
	area_size = 1000;

	fbo = NULL;
	shadowmap = NULL;
	light_camera = NULL;
}

void GTR::LightEntity::configure(cJSON* json)
{
	color = readJSONVector3(json, "color", Vector3());
	intensity = readJSONNumber(json, "intensity", 0);
	max_dist = readJSONNumber(json, "max_dist", 0);
	cone_angle = readJSONNumber(json, "cone_angle", 0);
	cone_exp = readJSONNumber(json, "cone_exp", 0);
	area_size = readJSONNumber(json, "area_size", 0);
	cast_shadows = readJSONBool(json, "cast_shadows", false);
	shadow_bias = readJSONNumber(json, "shadow_bias", 0);

	std::string str = readJSONString(json, "light_type", "");
	if (str == "POINT")
		light_type = eLightType::POINT;
	else if (str == "SPOT")
		light_type = eLightType::SPOT;
	else if (str == "DIRECTIONAL")
		light_type = eLightType::DIRECTIONAL;
}

void GTR::LightEntity::renderInMenu()
{
	BaseEntity::renderInMenu();

#ifndef SKIP_IMGUI

	std::string type_str;
	switch (light_type)
	{
		case eLightType::POINT: type_str = "POINT"; break;
		case eLightType::SPOT: type_str = "SPOT"; break;
		case eLightType::DIRECTIONAL: type_str = "DIRECTIONAL"; break;
	}

	ImGui::Text("LightType: %s", type_str.c_str()); //Edit 3 floats representing a color

	if (light_type == SPOT)
	{
		ImGui::DragFloat("Cone_angle", &cone_angle);
		ImGui::DragFloat("Cone_exp", &cone_exp);
		ImGui::Checkbox("Cast_shadow", &cast_shadows);
	}

	if (light_type == DIRECTIONAL)
	{
		ImGui::DragFloat("Area_size", &area_size);
		ImGui::Checkbox("Cast_shadow", &cast_shadows);
	}

	ImGui::ColorEdit3("Color", color.v);
	ImGui::DragFloat("Intensity", &intensity);
	ImGui::DragFloat("Max_dist", &max_dist);
	ImGui::DragFloat("Shadow_bias", &shadow_bias);
	
#endif
}

GTR::ReflectionProbeEntity::ReflectionProbeEntity()
{
	entity_type = REFLECTION_PROBE;
	texture = NULL;
}