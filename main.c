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
#define MAX_TYPE_FILES 420420
#define MAX_I32_MAP_ENTRIES 420

typedef int32_t i32;
typedef uint32_t u32;

enum entity_type {
	OBJECT_GUN,
	OBJECT_BULLET,
	OBJECT_GROUND,
};

struct i32_map {
	char *keys[MAX_I32_MAP_ENTRIES];
	i32 values[MAX_I32_MAP_ENTRIES];

	u32 buckets[MAX_I32_MAP_ENTRIES];
	u32 chains[MAX_I32_MAP_ENTRIES];

	size_t size;
};

struct entity {
	i32 id;
	enum entity_type type;
	b2BodyId body_id;
	b2ShapeId shape_id;
	Texture texture;

	// This has ownership, because grug_resource_reloads[] contains texture paths
	// that start dangling the moment the .so is unloaded
	// There is no way for `streq(entity->texture_path, reload.path)` to work without ownership
	char *texture_path;

	bool flippable;
	bool enable_hit_events;

	struct i32_map *i32_map;
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
static size_t drawn_entities;

static b2WorldId world_id;

static Texture background_texture;

struct measurement {
	struct timespec time;
	char *description;
};

static struct gun gun_definition;
static struct bullet bullet_definition;
static struct box box_definition;

static struct entity *gun;

static struct grug_file *type_files[MAX_TYPE_FILES];
static size_t type_files_size;

static bool draw_bounding_box = false;

static i32 next_entity_id;

static Sound metal_blunt_1;
static Sound metal_blunt_2;

static size_t sound_cooldown_metal_blunt_1;
static size_t sound_cooldown_metal_blunt_2;

// TODO: Optimize this to O(1), by adding an array that maps
// TODO: the entity ID to the entities[] index
static size_t get_entity_index_from_entity_id(i32 id) {
	for (size_t i = 0; i < entities_size; i++) {
		if (entities[i].id == id) {
			return i;
		}
	}

	return SIZE_MAX;
}

// From https://sourceware.org/git/?p=binutils-gdb.git;a=blob;f=bfd/elf.c#l193
static u32 elf_hash(const char *namearg) {
	u32 h = 0;

	for (const unsigned char *name = (const unsigned char *) namearg; *name; name++) {
		h = (h << 4) + *name;
		h ^= (h >> 24) & 0xf0;
	}

	return h & 0x0fffffff;
}

static bool streq(char *a, char *b) {
	return strcmp(a, b) == 0;
}

void game_fn_map_set_i32(i32 id, char *key, i32 value) {
	size_t entity_index = get_entity_index_from_entity_id(id);
	if (entity_index == SIZE_MAX) {
		return;
	}

	struct i32_map *map = entities[entity_index].i32_map;

	u32 bucket_index = elf_hash(key) % MAX_I32_MAP_ENTRIES;

	u32 i = map->buckets[bucket_index];

	while (true) {
		if (i == UINT32_MAX) {
			break;
		}

		if (streq(key, map->keys[i])) {
			break;
		}

		i = map->chains[i];
	}

	if (i == UINT32_MAX) {
		if (map->size >= MAX_I32_MAP_ENTRIES) {
			return;
		}

		i = map->size;

		map->keys[i] = strdup(key);
		map->values[i] = value;
		map->chains[i] = map->buckets[bucket_index];
		map->buckets[bucket_index] = i;

		map->size++;
	} else {
		free(map->keys[i]);
		map->keys[i] = strdup(key);
		map->values[i] = value;
	}
}

i32 game_fn_map_get_i32(i32 id, char *key) {
	size_t entity_index = get_entity_index_from_entity_id(id);
	if (entity_index == SIZE_MAX) {
		return -1;
	}

	struct i32_map *map = entities[entity_index].i32_map;

	if (map->size == 0) {
		return -1;
	}

	u32 i = map->buckets[elf_hash(key) % MAX_I32_MAP_ENTRIES];

	while (true) {
		if (i == UINT32_MAX) {
			break;
		}

		if (streq(key, map->keys[i])) {
			return map->values[i];
		}

		i = map->chains[i];
	}

	return -1;
}

bool game_fn_map_has_i32(i32 id, char *key) {
	size_t entity_index = get_entity_index_from_entity_id(id);
	if (entity_index == SIZE_MAX) {
		return false;
	}

	struct i32_map *map = entities[entity_index].i32_map;

	if (map->size == 0) {
		return false;
	}

	u32 i = map->buckets[elf_hash(key) % MAX_I32_MAP_ENTRIES];

	while (true) {
		if (i == UINT32_MAX) {
			break;
		}

		if (streq(key, map->keys[i])) {
			return true;
		}

		i = map->chains[i];
	}

	return false;
}

void game_fn_play_sound(char *path) {
	Sound sound = LoadSound(path);
	assert(sound.frameCount > 0);

	PlaySound(sound);

	// TODO: This doesn't work here, since it frees the sound before it gets played
	// UnloadSound(sound);
}

float game_fn_rand(float min, float max) {
    float range = max - min;
    return min + rand() / (double)RAND_MAX * range;
}

void game_fn_define_box(char *name, char *sprite_path, bool static_) {
	box_definition = (struct box){
		.name = name,
		.sprite_path = sprite_path,
		.static_ = static_,
	};
}

void game_fn_define_bullet(char *name, char *sprite_path) {
	bullet_definition = (struct bullet){
		.name = name,
		.sprite_path = sprite_path,
	};
}

void game_fn_define_gun(char *name, char *sprite_path) {
	gun_definition = (struct gun){
		.name = name,
		.sprite_path = sprite_path,
	};
}

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

	bool facing_left = (angle > PI / 2) || (angle < -PI / 2);
    Rectangle source = { 0.0f, 0.0f, (float)texture.width, (float)texture.height * (entity.flippable && facing_left ? -1 : 1) };
    Rectangle dest = { pos_screen.x, pos_screen.y, (float)texture.width*TEXTURE_SCALE, (float)texture.height*TEXTURE_SCALE };
    Vector2 origin = { 0.0f, 0.0f };
	float rotation = -angle * RAD2DEG;
	DrawTexturePro(texture, source, dest, origin, rotation, WHITE);

	if (draw_bounding_box) {
		Rectangle rect = {pos_screen.x, pos_screen.y, texture.width * TEXTURE_SCALE, texture.height * TEXTURE_SCALE};
		Color color = {.r=42, .g=42, .b=242, .a=100};
		DrawRectanglePro(rect, origin, -angle * RAD2DEG, color);
	}

	drawn_entities++;
}

static void draw(void) {
	BeginDrawing();

	DrawTextureEx(background_texture, Vector2Zero(), 0, 2, WHITE);

	drawn_entities = 0;
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

		free(entities[entity_index].texture_path);

		b2DestroyBody(entities[entity_index].body_id);
	}

	struct i32_map *map = entities[entity_index].i32_map;
	for (size_t i = 0; i < map->size; i++) {
		free(map->keys[i]);
	}
	free(map);

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
	// printf("approachSpeed: %f\n", event->approachSpeed);

	float x_normalized = (event->point.x * TEXTURE_SCALE) / (SCREEN_WIDTH / 2); // Between -1.0f and 1.0f
	// printf("x_normalized: %f\n", x_normalized);

	float y_normalized = (event->point.y * TEXTURE_SCALE) / (SCREEN_HEIGHT / 2); // Between -1.0f and 1.0f
	// printf("y_normalized: %f\n", y_normalized);

	float distance = sqrtf(x_normalized * x_normalized + y_normalized * y_normalized);
	// printf("distance: %f\n", distance);

	float audibility = 1.0f;
	if (distance > 0.0f) { // Prevents a later division by 0.0f
		distance *= 5.0f;

		// This considers the game to be a 3D space
		// See https://en.wikipedia.org/wiki/Inverse-square_law
		audibility = 1.0f / (distance * distance); // Between 0.0f and 1.0f

		// This considers the game to be a 2D space
		// audibility = 1.0f / distance; // Between 0.0f and 1.0f

		assert(audibility >= 0.0f);
	}
	// printf("audibility: %f\n", audibility);

	float volume = event->approachSpeed * 0.01f;

	volume *= audibility;

	if (volume > 1.0f) {
		volume = 1.0f;
	}
	if (volume < 0.01f) {
		return;
	}

	Sound sound;
	if (rand() % 2 == 0 && sound_cooldown_metal_blunt_1 == 0) {
		sound = metal_blunt_1;
		sound_cooldown_metal_blunt_1 = 6;
		// sound_volume_metal_blunt_1 = ;
	} else if (sound_cooldown_metal_blunt_2 == 0) {
		sound = metal_blunt_2;
		sound_cooldown_metal_blunt_2 = 6;
		// sound_volume_metal_blunt_2 = ;
	} else {
		return;
	}

	SetSoundVolume(sound, volume);

	float speed = event->approachSpeed * 0.005f;
	// printf("speed: %f\n", speed);
	float min_pitch = 0.5f;
	float max_pitch = 1.5f;
	float pitch = min_pitch + speed;
	// printf("pitch: %f\n", pitch);
	if (pitch > max_pitch) {
		pitch = max_pitch;
	}
	SetSoundPitch(sound, pitch);

	float x_normalized_inverted = -x_normalized; // Because a pan of 1.0f means all the way left, instead of right
	float pan = 0.5f + x_normalized_inverted / 2.0f; // Between 0.0f and 1.0f
	// printf("pan: %f\n", pan);
	SetSoundPan(sound, pan);

	PlaySound(sound);
}

static b2ShapeId add_shape(b2BodyId body_id, Texture texture, bool enable_hit_events) {
	b2ShapeDef shape_def = b2DefaultShapeDef();

	shape_def.enableHitEvents = enable_hit_events;

	b2Polygon polygon = b2MakeBox(texture.width / 2.0f, texture.height / 2.0f);

	return b2CreatePolygonShape(body_id, &shape_def, &polygon);
}

static char *get_texture_path(enum entity_type entity_type) {
	switch (entity_type) {
		case OBJECT_GUN:
			return "mods/vanilla/m60/m60.png";
		case OBJECT_BULLET:
			return "mods/vanilla/m60/pg-7vl.png";
		case OBJECT_GROUND:
			return "mods/vanilla/concrete.png";
	}
	return NULL;
}

static i32 spawn_entity(b2BodyDef body_def, enum entity_type type, bool flippable, bool enable_hit_events) {
	if (entities_size >= MAX_ENTITIES) {
		return -1;
	}

	struct entity *entity = &entities[entities_size];

	*entity = (struct entity){0};

	entity->type = type;

	char *texture_path = get_texture_path(type);

	body_def.userData = (void *)entities_size;

	entity->body_id = b2CreateBody(world_id, &body_def);

	entity->flippable = flippable;

	entity->enable_hit_events = enable_hit_events;

	entity->texture = LoadTexture(texture_path);
	assert(entity->texture.id > 0);

	entity->texture_path = strdup(texture_path);

	entity->shape_id = add_shape(entity->body_id, entity->texture, enable_hit_events);

	entity->i32_map = malloc(sizeof(*entity->i32_map));
	memset(entity->i32_map->buckets, 0xff, MAX_I32_MAP_ENTRIES * sizeof(u32));
	entity->i32_map->size = 0;

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

	spawn_entity(body_def, OBJECT_BULLET, false, true);
}

static struct entity *spawn_gun(b2Vec2 pos) {
	b2BodyDef body_def = b2DefaultBodyDef();
	body_def.position = pos;

	struct entity *gun_entity = entities + entities_size;

	spawn_entity(body_def, OBJECT_GUN, true, false);

	return gun_entity;
}

static void spawn_ground(void) {
	int ground_entity_count = 16;

	char *texture_path = get_texture_path(OBJECT_GROUND);
	Texture texture = LoadTexture(texture_path);
	assert(texture.id > 0);

	for (int i = 0; i < ground_entity_count; i++) {
		b2BodyDef body_def = b2DefaultBodyDef();
		body_def.position = (b2Vec2){ (i - ground_entity_count / 2) * texture.width, -100.0f };

		spawn_entity(body_def, OBJECT_GROUND, false, false);
	}

	UnloadTexture(texture);
}

static void push_file_containing_fn(struct grug_file *file) {
	if (type_files_size + 1 > MAX_TYPE_FILES) {
		fprintf(stderr, "There are more than %d files containing the requested type, exceeding MAX_TYPE_FILES", MAX_TYPE_FILES);
		exit(EXIT_FAILURE);
	}
	type_files[type_files_size++] = file;
}

static void update_type_files_impl(struct grug_mod_dir dir, char *define_type) {
	for (size_t i = 0; i < dir.dirs_size; i++) {
		update_type_files_impl(dir.dirs[i], define_type);
	}
	for (size_t i = 0; i < dir.files_size; i++) {
		if (streq(define_type, dir.files[i].define_type)) {
			push_file_containing_fn(&dir.files[i]);
		}
	}
}

static struct grug_file **get_type_files(char *define_type) {
	type_files_size = 0;
	update_type_files_impl(grug_mods, define_type);
	return type_files;
}

static void update(void) {
	if (grug_mod_had_runtime_error()) {
		fprintf(stderr, "Runtime error: %s\n", grug_get_runtime_error_reason());
		fprintf(stderr, "Error occurred when the game called %s(), from %s\n", grug_on_fn_name, grug_on_fn_path);

		draw();
		return;
	}

	if (grug_regenerate_modified_mods()) {
		fprintf(stderr, "Loading error: %s:%d: %s (grug.c:%d)\n", grug_error.path, grug_error.line_number, grug_error.msg, grug_error.grug_c_line_number);

		draw();
		return;
	}

	struct grug_file **box_files = get_type_files("box");

	struct grug_file *concrete_file = NULL;
	// Use the first static box
	for (size_t i = 0; i < type_files_size; i++) {
		box_files[i]->define_fn();
		if (box_definition.static_) {
			concrete_file = box_files[i];
			break;
		}
	}
	assert(concrete_file && "There must be at least one static type of box, cause we want to form a floor");

	struct grug_file *crate_file = NULL;
	// Use the first non-static box
	for (size_t i = 0; i < type_files_size; i++) {
		box_files[i]->define_fn();
		if (!box_definition.static_) {
			crate_file = box_files[i];
			break;
		}
	}
	assert(crate_file && "There must be at least one non-static type of box, cause we want to have crates that can fall down");

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

	if (sound_cooldown_metal_blunt_1 > 0) {
		sound_cooldown_metal_blunt_1--;
	}
	if (sound_cooldown_metal_blunt_2 > 0) {
		sound_cooldown_metal_blunt_2--;
	}
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
		char *texture_path = get_texture_path(OBJECT_BULLET);

		Texture texture = LoadTexture(texture_path);
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
	metal_blunt_2 = LoadSound("MetalBlunt2.wav");
	assert(metal_blunt_2.frameCount > 0);

	while (!WindowShouldClose()) {
		update();
	}

	UnloadTexture(background_texture);
	for (size_t i = 0; i < entities_size; i++) {
		UnloadTexture(entities[i].texture);
	}
	UnloadSound(metal_blunt_1);
	UnloadSound(metal_blunt_2);
	CloseAudioDevice();
	CloseWindow();
}
