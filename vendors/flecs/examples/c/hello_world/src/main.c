#include <hello_world.h>

typedef struct {
    double x;
    double y;
} Position, Velocity;

/* Move system implementation. System callbacks may be called multiple times,
 * as entities are grouped by which components they have, and each group has
 * its own set of component arrays. */
void Move(ecs_iter_t *it) {
    Position *p = ecs_term(it, Position, 1);
    Velocity *v = ecs_term(it, Velocity, 2);

    /* Print the set of components for the iterated over entities */
    char *type_str = ecs_type_str(it->world, it->type);
    printf("Move entities with [%s]\n", type_str);
    ecs_os_free(type_str);

    /* Iterate entities for the current group */
    for (int i = 0; i < it->count; i ++) {
        p[i].x += v[i].x;
        p[i].y += v[i].y;
    }
}

int main(int argc, char *argv[]) {
    /* Create the world */
    ecs_world_t *world = ecs_init_w_args(argc, argv);

    /* Register components */
    ECS_COMPONENT(world, Position);
    ECS_COMPONENT(world, Velocity);

    /* Register system */
    ECS_SYSTEM(world, Move, EcsOnUpdate, Position, Velocity);

    /* Register tags (components without a size) */
    ECS_TAG(world, Eats);
    ECS_TAG(world, Apples);
    ECS_TAG(world, Pears);

    /* Create an entity with name Bob, add Position and food preference */
    ecs_entity_t Bob = ecs_set_name(world, 0, "Bob");
    ecs_set(world, Bob, Position, {0, 0});
    ecs_set(world, Bob, Velocity, {1, 2});
    ecs_add_pair(world, Bob, Eats, Apples);

    /* Run systems twice. Usually this function is called once per frame */
    ecs_progress(world, 0);
    ecs_progress(world, 0);

    /* See if Bob has moved (he has) */
    const Position *p = ecs_get(world, Bob, Position);
    printf("Bob's position is {%f, %f}\n", p->x, p->y);

    return ecs_fini(world);
}
