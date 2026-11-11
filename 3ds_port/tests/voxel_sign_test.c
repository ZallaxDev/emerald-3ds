/* Pixel geometry and real baked sidecar. Never writes into ROMFS. */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "voxel_sign.h"
#include "voxel_regions.h"
#include "voxel_relief.h"
#include "voxel_building.h"

static bool sFloor;
static unsigned sLayout;
static int sX, sY;
static int sOriginY;
static unsigned sRole = VOXEL_ROLE_SIGNPOST;

void VoxelBuilder_Quad(VoxelBuilder *b, const VoxelVertex *a, const VoxelVertex *c,
                       const VoxelVertex *d, const VoxelVertex *e)
{
    assert(b->count + 6 <= b->capacity);
    b->vertices[b->count++] = *a; b->vertices[b->count++] = *c;
    b->vertices[b->count++] = *d; b->vertices[b->count++] = *a;
    b->vertices[b->count++] = *d; b->vertices[b->count++] = *e;
}
void VoxelMesh_Top(VoxelBuilder *b,float x,float z,float h,float inset,
                    float u,float v,float ue,float ve,float shade)
{
    (void)b; (void)x; (void)z; (void)h; (void)inset;
    (void)v; (void)ue; (void)ve; (void)shade;
    assert(u == .5f); /* donor, not the board */
    sFloor = true;
}
/* No atlas slot for the matched ground: the cell south of the sign is laid. */
bool VoxelMesh_MetatileUV(VoxelBuilder *b,int m,float *u,float *v,float *ue,float *ve)
{
    (void)b; (void)m; (void)u; (void)v; (void)ue; (void)ve;
    return false;
}
bool VoxelMesh_TileUV(VoxelBuilder *b,int x,int y,float *u,float *v,float *ue,float *ve)
{
    (void)b; (void)x;
    *u = y == sY+sOriginY+1 ? .5f : y == sY+sOriginY-1 ? .75f : 0;
    *ue = *u + .25f;
    *v = 1; *ve = .75f;
    return true;
}
/* Signs here stand on level ground: no relief under them. */
const int16_t *VoxelRelief_Cell(const VoxelMapInstance *inst, int x, int y)
{
    (void)inst; (void)x; (void)y;
    return NULL;
}
/* These fixtures have no relief grid, depth map or drawn slopes. */
const int16_t *VoxelRelief_Depth(const VoxelMapInstance *inst, int x, int y)
{
    (void)inst; (void)x; (void)y;
    return NULL;
}

bool VoxelRelief_IsDrawn(const VoxelMapInstance *inst)
{
    (void)inst;
    return false;
}

bool VoxelRelief_IsSlope(const int16_t *grid)
{
    (void)grid;
    return false;
}

/* Nor on a bridge's deck. */
bool VoxelBuildings_DeckAt(const VoxelMapInstance *inst, float worldX, float worldZ,
                           unsigned elevation, float *lift)
{
    (void)inst; (void)worldX; (void)worldZ; (void)elevation; (void)lift;
    return false;
}
float VoxelRelief_Base(const VoxelMapInstance *inst)
{
    (void)inst;
    return 0.0f;
}
float VoxelRelief_CellLift(const VoxelMapInstance *inst, int x, int y)
{
    (void)inst; (void)x; (void)y;
    return 0.0f;
}
float VoxelRelief_CellShift(const VoxelMapInstance *inst, int x, int y)
{
    (void)inst; (void)x; (void)y;
    return 0.0f;
}
unsigned VoxelRegions_RoleAt(unsigned id,int x,int y)
{
    return id == sLayout && x == sX && y == sY ? sRole : VOXEL_ROLE_FLOOR;
}

static void CheckDewford(void)
{
    static VoxelVertex vertices[16384];
    VoxelBuilder builder = {.vertices=vertices,.capacity=16384};
    VoxelMapInstance inst = {.layoutId=12,.originX=7,.originY=9};
    int ground = -1;
    bool foot = false, top = false, headUV = false, bodyUV = false;
    sLayout = 12;
    sX = 11;
    sY = 16;
    sOriginY = inst.originY;
    sRole = VOXEL_ROLE_WALL;
    int xpos = sX + inst.originX, ypos = sY + inst.originY;
    assert(VoxelRegions_RoleAt(sLayout, sX, sY) == VOXEL_ROLE_WALL);
    assert(VoxelSign_IsCell(&inst, xpos, ypos));
    assert(!VoxelSign_IsCell(&inst, xpos, ypos - 1));
    assert(!VoxelSign_IsCell(&inst, xpos - 1, ypos));
    assert(VoxelSign_HeadGround(&inst, xpos, ypos - 1, &ground) && ground == 0x124);
    assert(!VoxelSign_HeadGround(&inst, xpos, ypos, NULL));
    assert(VoxelSign_EmitCell(&builder, &inst, xpos, ypos));
    assert(sFloor && builder.count > 0 && builder.dropped == 0);
    for (unsigned index = 0; index < builder.count; ++index)
    {
        VoxelVertex *vertex = &vertices[index];
        assert(vertex->x >= xpos + 1.0f / 16 && vertex->x <= xpos + 1.0f);
        assert(vertex->y >= 0.0f && vertex->y <= 23.0f / 16);
        assert(vertex->z == ypos + .6875f || vertex->z == ypos + .8125f);
        if (vertex->y == 0.0f) foot = true;
        if (vertex->y == 23.0f / 16) top = true;
        if (vertex->y > 1.0f)
        {
            assert(vertex->u >= .75f && vertex->u <= 1.0f);
            headUV = true;
        }
        else if (vertex->y < 1.0f)
        {
            assert(vertex->u >= 0.0f && vertex->u <= .25f);
            bodyUV = true;
        }
    }
    assert(foot && top && headUV && bodyUV);
    assert(VoxelSign_CasterTop(&inst, xpos, ypos) == 23.0f / 16);
    assert(VoxelSign_Occludes(&inst, xpos, ypos, xpos + .5f, 1.25f, ypos + .75f));
    assert(!VoxelSign_Occludes(&inst, xpos, ypos, xpos + .5f, 1.5f, ypos + .75f));
    VoxelBuilder glass = {.vertices=vertices,.capacity=16384};
    VoxelBuilder nightGround = {.vertices=vertices,.capacity=16384};
    VoxelSign_EmitNight(&glass, &nightGround, &inst, xpos, ypos, xpos + 1, ypos + 1);
    assert(glass.count == 0 && nightGround.count == 0);
    builder.count = 0;
    sFloor = false;
    assert(VoxelSign_EmitStanding(&builder, &inst, xpos, ypos));
    assert(!sFloor && builder.count > 0 && builder.dropped == 0);
    VoxelSign_Shutdown();
    puts("PASS Dewford gym sign: real VXS3, wall-role override, 23-pixel board, head UV, ground, no lamp glow");
}

int main(int argc,char **argv)
{
    if (argc == 3 && strcmp(argv[2], "--dewford") == 0)
    {
        CheckDewford();
        return 0;
    }
    static VoxelVertex vertices[4096];
    VoxelBuilder b = {.vertices=vertices,.capacity=4096};
    VoxelSignMask m = {.width=16,.height=16,.u0=.25f,.v0=1,
                       .du=1.0f/512,.dv=-1.0f/256};
    /* Board whose last painted row is NOT the metatile's last row. Previously
     * the front floated above the side faces, and SOUTH reversed its text. */
    for (int y=2;y<=12;++y) m.opaque[y]=0x3FFC;
    VoxelSign_Emit(&b,2,3,0,&m,VOXEL_SIGN_PINNED_DEPTH,1);
    assert(b.count > 0 && b.count < 132*12);
    bool base=false, front=false, back=false;
    for (unsigned i=0;i<b.count;++i)
    {
        VoxelVertex *v=&vertices[i];
        assert(v->y>=0 && v->y<=11.0f/16);
        assert(v->z==3.6875f || v->z==3.8125f);
        assert(v->u>=m.u0+2*m.du && v->u<=m.u0+14*m.du);
        if (v->y==0) base=true;
    }
    for (unsigned i=0;i<b.count;i+=6)
    {
        VoxelVertex *a=&vertices[i], *c=&vertices[i+1];
        if (a->z==c->z && a->x!=c->x)
        {
            if (a->z==3.8125f) { assert(a->x<c->x && a->u<c->u); front=true; }
            if (a->z==3.6875f) back=true;
        }
    }
    assert(base && front && back);
    /* A lamp: a post in the sign's cell, its lantern in the cell north. The
     * lantern stands on the post, textured from its own cell. */
    {
        VoxelSignMask l = {.width=16,.height=32,.headRows=16,.u0=.25f,.v0=1,
                           .du=1.0f/512,.dv=-1.0f/256,.headU0=.5f,.headV0=.5f};
        bool top=false;
        for (int y=4;y<32;++y) l.opaque[y]=0x0380;
        b.count=0;
        VoxelSign_Emit(&b,2,3,0,&l,VOXEL_SIGN_PINNED_DEPTH,1);
        for (unsigned i=0;i<b.count;++i)
        {
            VoxelVertex *v=&vertices[i];
            assert(v->y>=0 && v->y<=28.0f/16);
            assert(v->z==4.6875f || v->z==4.8125f);
            if (v->y>1.0f) { assert(v->u>=.5f && v->v<=.5f); top=true; }
            else if (v->y<.99f) assert(v->u<.5f && v->v<=1 && v->v>=.75f);
        }
        assert(top);
        b.count=0;
    }
    /* Runtime consumes the very file shipped by the bake, not a fake resource
     * written over it by the test. The first record identifies our fixture. */
    assert(argc==2);
    FILE *f=fopen(argv[1],"rb");
    unsigned char header[8], key[6];
    assert(f && fread(header,1,8,f)==8 && memcmp(header,"VXS3",4)==0);
    assert(fread(key,1,6,f)==6); fclose(f);
    sLayout=key[0]|(key[1]<<8); sX=key[2]|(key[3]<<8); sY=key[4]|(key[5]<<8);
    VoxelMapInstance inst={.layoutId=(int)sLayout};
    b.count=0;
    assert(VoxelSign_EmitCell(&b,&inst,sX,sY));
    assert(sFloor && b.count>0 && b.dropped==0);
    assert(!VoxelSign_EmitCell(&b,&inst,sX+1,sY));
    VoxelSign_Shutdown();
    puts("PASS signs: board paint, two-pixel depth, UV orientation, grounded shell, ROMFS runtime");
    return 0;
}
