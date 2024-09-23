// So VS Code can find "CLOCK_PROCESS_CPUTIME_ID", and so we can use strdup()
#define _POSIX_C_SOURCE 200809L

#include "box2d/box2d.h"
#include "grug.h"
#include "raylib.h"
#include "raymath.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define SCREEN_WIDTH 1280
#define SCREEN_HEIGHT 720
#define TEXTURE_SCALE 2.0f
#define PIXELS_PER_METER 20.0f // Taken from Cortex Command, where this program's sprites come from: https://github.com/cortex-command-community/Cortex-Command-Community-Project/blob/afddaa81b6d71010db299842d5594326d980b2cc/Source/System/Constants.h#L23
#define MAX_ENTITIES 1000 // Prevents box2d crashing when there's more than 32k overlapping entities, which can happen when the game is paused and the player shoots over 32k bullets

typedef int32_t i32;
typedef uint32_t u32;

enum entity_type {
	OBJECT_GUN,
	OBJECT_BULLET,
	OBJECT_GROUND,
};

struct entity {
	i32 id;
	enum entity_type type;
	b2BodyId body_id;
	b2ShapeId shape_id;
	Texture texture;

	bool enable_hit_events;
};

struct gun {
	char *name;
	char *sprite_path;
};

struct bullet {
	char *name;
	char *sprite_path;
};

struct box {
	char *name;
	char *sprite_path;
	bool static_;
};

static struct entity entities[MAX_ENTITIES];
static size_t entities_size;

static b2WorldId world_id;

static Texture background_texture;

static struct entity *gun;

static i32 next_entity_id;

static Sound metal_blunt_1;

static Vector2 world_to_screen(b2Vec2 p) {
	return (Vector2){
		  p.x * TEXTURE_SCALE + SCREEN_WIDTH  / 2.0f,
		- p.y * TEXTURE_SCALE + SCREEN_HEIGHT / 2.0f
	};
}

static void draw_entity(struct entity entity) {
	Texture texture = entity.texture;

	b2Vec2 local_point = {
		-texture.width / 2.0f,
		texture.height / 2.0f
	};

	// Rotates the local_point argument by the entity's angle
	b2Vec2 pos_world = b2Body_GetWorldPoint(entity.body_id, local_point);

	Vector2 pos_screen = world_to_screen(pos_world);

	// Using this would be more accurate for huge textures, but would probably be slower
	// b2AABB aabb = b2Body_ComputeAABB(entity.body_id);
	// Vector2 lower = world_to_screen(aabb.lowerBound);
	// Vector2 upper = world_to_screen(aabb.upperBound);

	float margin = -2.0f * PIXELS_PER_METER;
	float left = pos_screen.x + margin;
	float right = pos_screen.x - margin;
	float top = pos_screen.y + margin;
	float bottom = pos_screen.y - margin;
	if (left > SCREEN_WIDTH || right < 0 || top > SCREEN_HEIGHT || bottom < 0) {
		return;
	}

	b2Rot rot = b2Body_GetRotation(entity.body_id);
	float angle = b2Rot_GetAngle(rot);

    Rectangle source = { 0.0f, 0.0f, (float)texture.width, (float)texture.height };
    Rectangle dest = { pos_screen.x, pos_screen.y, (float)texture.width*TEXTURE_SCALE, (float)texture.height*TEXTURE_SCALE };
    Vector2 origin = { 0.0f, 0.0f };
	float rotation = -angle * RAD2DEG;
	DrawTexturePro(texture, source, dest, origin, rotation, WHITE);

	Rectangle rect = {pos_screen.x, pos_screen.y, texture.width * TEXTURE_SCALE, texture.height * TEXTURE_SCALE};
	Color color = {.r=42, .g=42, .b=242, .a=100};
	DrawRectanglePro(rect, origin, -angle * RAD2DEG, color);
}

static void draw(void) {
	BeginDrawing();

	DrawTextureEx(background_texture, Vector2Zero(), 0, 2, WHITE);

	for (size_t i = 0; i < entities_size; i++) {
		struct entity entity = entities[i];

		if (entity.texture.id > 0) {
			draw_entity(entity);
		}
	}

	EndDrawing();
}

static void despawn_entity(size_t entity_index) {
	if (entities[entity_index].texture.id > 0) {
		UnloadTexture(entities[entity_index].texture);

		b2DestroyBody(entities[entity_index].body_id);
	}

	entities[entity_index] = entities[--entities_size];

	if (entities[entity_index].type == OBJECT_GUN) {
		gun = entities + entity_index;
	}

	// If the removed entity wasn't at the very end of the entities array,
	// update entity_index's userdata
	if (entity_index < entities_size && entities[entity_index].texture.id > 0) {
		b2Body_SetUserData(entities[entity_index].body_id, (void *)entity_index);
	}
}

static void play_collision_sound(b2ContactHitEvent *event) {
	float volume = event->approachSpeed * 0.01f;

	if (volume > 1.0f) {
		volume = 1.0f;
	}
	if (volume < 0.01f) {
		return;
	}

	SetSoundVolume(metal_blunt_1, volume);

	PlaySound(metal_blunt_1);
}

static b2ShapeId add_shape(b2BodyId body_id, Texture texture, bool enable_hit_events) {
	b2ShapeDef shape_def = b2DefaultShapeDef();

	shape_def.enableHitEvents = enable_hit_events;

	b2Polygon polygon = b2MakeBox(texture.width / 2.0f, texture.height / 2.0f);

	return b2CreatePolygonShape(body_id, &shape_def, &polygon);
}

static i32 spawn_entity(b2BodyDef body_def, enum entity_type type, bool enable_hit_events) {
	if (entities_size >= MAX_ENTITIES) {
		return -1;
	}

	struct entity *entity = &entities[entities_size];

	*entity = (struct entity){0};

	entity->type = type;

	body_def.userData = (void *)entities_size;

	entity->body_id = b2CreateBody(world_id, &body_def);

	entity->enable_hit_events = enable_hit_events;

	char *texture_path = NULL;
	switch (type) {
		case OBJECT_GUN:
			texture_path = "m60.png";
			break;
		case OBJECT_BULLET:
			texture_path = "pg-7vl.png";
			break;
		case OBJECT_GROUND:
			texture_path = "concrete.png";
			break;
	}

	entity->texture = LoadTexture(texture_path);
	assert(entity->texture.id > 0);

	entity->shape_id = add_shape(entity->body_id, entity->texture, enable_hit_events);

	entity->id = next_entity_id;
	if (entity->id == INT32_MAX) {
		next_entity_id = 0;
	} else {
		next_entity_id++;
	}

	entities_size++;

	return entity->id;
}

static void spawn_bullet(b2Vec2 pos, float angle, b2Vec2 velocity) {
	b2BodyDef body_def = b2DefaultBodyDef();
	body_def.type = b2_dynamicBody;
	body_def.position = pos;
	body_def.rotation = b2MakeRot(angle);
	body_def.linearVelocity = velocity;

	spawn_entity(body_def, OBJECT_BULLET, true);
}

static struct entity *spawn_gun(b2Vec2 pos) {
	b2BodyDef body_def = b2DefaultBodyDef();
	body_def.position = pos;

	struct entity *gun_entity = entities + entities_size;

	spawn_entity(body_def, OBJECT_GUN, false);

	return gun_entity;
}

static void spawn_ground(void) {
	int ground_entity_count = 16;

	Texture texture = LoadTexture("concrete.png");
	assert(texture.id > 0);

	for (int i = 0; i < ground_entity_count; i++) {
		b2BodyDef body_def = b2DefaultBodyDef();
		body_def.position = (b2Vec2){ (i - ground_entity_count / 2) * texture.width, -100.0f };

		spawn_entity(body_def, OBJECT_GROUND, false);
	}

	UnloadTexture(texture);
}

static void update(void) {
	static bool initialized = false;
	if (!initialized) {
		initialized = true;

		b2Vec2 pos = { 100.0f, 0 };

		gun = spawn_gun(pos);

		spawn_ground();
	}

	if (IsKeyPressed(KEY_C)) { // Clear bullets
		for (size_t i = entities_size; i > 0; i--) {
			enum entity_type type = entities[i - 1].type;
			if (type == OBJECT_BULLET) {
				despawn_entity(i - 1);
			}
		}
	}

	float deltaTime = GetFrameTime();
	b2World_Step(world_id, deltaTime, 4);

	b2ContactEvents contactEvents = b2World_GetContactEvents(world_id);
	for (i32 i = 0; i < contactEvents.hitCount; i++) {
		b2ContactHitEvent *event = &contactEvents.hitEvents[i];
		printf("Hit event!\n");
		play_collision_sound(event);
	}

	Vector2 mouse_pos = GetMousePosition();
	b2Vec2 gun_world_pos = b2Body_GetPosition(gun->body_id);
	Vector2 gun_screen_pos = world_to_screen(gun_world_pos);
	Vector2 gun_to_mouse = Vector2Subtract(mouse_pos, gun_screen_pos);
	double gun_angle = atan2(-gun_to_mouse.y, gun_to_mouse.x);

	b2Body_SetTransform(gun->body_id, gun_world_pos, b2MakeRot(gun_angle));

	if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
		Texture texture = LoadTexture("concrete.png");
		assert(texture.id > 0);

		b2Vec2 local_point = {
			.x = gun->texture.width / 2.0f + texture.width / 2.0f,
			.y = 0.0f
		};
		UnloadTexture(texture);
		b2Vec2 muzzle_pos = b2Body_GetWorldPoint(gun->body_id, local_point);

		b2Rot rot = b2MakeRot(gun_angle);

		b2Vec2 velocity_unrotated = (b2Vec2){.x=100.0f * PIXELS_PER_METER, .y=0.0f};
		b2Vec2 velocity = b2RotateVector(rot, velocity_unrotated);
		spawn_bullet(muzzle_pos, gun_angle, velocity);
	}

	draw();
}

int main(void) {
	SetConfigFlags(FLAG_VSYNC_HINT);
	InitWindow(SCREEN_WIDTH, SCREEN_HEIGHT, "box2d-raylib");

	b2SetLengthUnitsPerMeter(PIXELS_PER_METER);

	b2WorldDef world_def = b2DefaultWorldDef();
	world_def.gravity.y = -9.8f * PIXELS_PER_METER;
	world_id = b2CreateWorld(&world_def);

	background_texture = LoadTexture("background.png");
	assert(background_texture.id > 0);

	InitAudioDevice();

	metal_blunt_1 = LoadSound("MetalBlunt1.wav");
	assert(metal_blunt_1.frameCount > 0);

	while (!WindowShouldClose()) {
		update();
	}

	UnloadTexture(background_texture);
	for (size_t i = 0; i < entities_size; i++) {
		UnloadTexture(entities[i].texture);
	}
	UnloadSound(metal_blunt_1);
	CloseAudioDevice();
	CloseWindow();
}
