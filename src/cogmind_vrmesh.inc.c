/* Self-contained skinned GLB avatar, COGVR_AVATAR=<file.glb>. Render-thread owned.
 * No external buffers/images; <=64 joints, <=4 embedded PNGs, <=200k vertices.
 * Source artwork is private and never packaged. cgltf (MIT), stb_image (MIT/public domain).
 */
#ifdef _WIN32
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeclaration-after-statement"
#pragma GCC diagnostic ignored "-Wunused-function"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* Decoder libc operations use the shim's real SDL2 helpers (the shim links -nostdlib). */
static char *CogMesh_Strcpy(char *d,const char *s){char *r=d;while((*d++=*s++)!=0){}return r;}
static char *CogMesh_Strncpy(char *d,const char *s,size_t n){size_t i=0;for(;i<n&&s[i];i++)d[i]=s[i];for(;i<n;i++)d[i]=0;return d;}
static size_t CogMesh_Strcspn(const char *s,const char *reject){size_t i;for(i=0;s[i]&&!SDL20_strchr(reject,s[i]);i++){}return i;}
static FILE *CogMesh_Fopen(const char *p,const char *m){return (FILE*)SDL20_RWFromFile(p,m);}
static size_t CogMesh_Fread(void *p,size_t s,size_t n,FILE *f){SDL_RWops *rw=(SDL_RWops*)f;return rw->read(rw,p,s,n);}
static int CogMesh_Fclose(FILE *f){SDL_RWops *rw=(SDL_RWops*)f;return rw->close(rw);}
static int CogMesh_Fseek(FILE *f,long offset,int whence){SDL_RWops *rw=(SDL_RWops*)f;return rw->seek(rw,offset,whence)<0?-1:0;}
static long CogMesh_Ftell(FILE *f){SDL_RWops *rw=(SDL_RWops*)f;return (long)rw->seek(rw,0,RW_SEEK_CUR);}
#define CGLTF_MALLOC SDL20_malloc
#define CGLTF_FREE SDL20_free
#define CGLTF_ATOI SDL20_atoi
#define CGLTF_ATOF(s) SDL20_strtod(s,NULL)
#define CGLTF_ATOLL(s) SDL20_strtoll(s,NULL,10)
#define memcpy SDL20_memcpy
#define memmove SDL20_memmove
#define memset SDL20_memset
#define strlen SDL20_strlen
#define strchr SDL20_strchr
#define strrchr SDL20_strrchr
#define strstr SDL20_strstr
#define strncmp SDL20_strncmp
#define strcpy CogMesh_Strcpy
#define strncpy CogMesh_Strncpy
#define strcspn CogMesh_Strcspn
#define fopen CogMesh_Fopen
#define fread CogMesh_Fread
#define fclose CogMesh_Fclose
#define fseek CogMesh_Fseek
#define ftell CogMesh_Ftell
#define CGLTF_IMPLEMENTATION
#include "cgltf.h"
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_NO_THREAD_LOCALS
#define STBI_MALLOC SDL20_malloc
#define STBI_REALLOC SDL20_realloc
#define STBI_FREE SDL20_free
#include "stb_image.h"
#undef memcpy
#undef memmove
#undef memset
#undef strlen
#undef strchr
#undef strrchr
#undef strstr
#undef strncmp
#undef strcpy
#undef strncpy
#undef strcspn
#undef fopen
#undef fread
#undef fclose
#undef fseek
#undef ftell
#pragma GCC diagnostic pop

typedef struct CogMeshVertex { float p[3], n[3], uv[2]; Uint16 j[4]; float w[4]; } CogMeshVertex;
typedef struct CogMeshCB { CogM4 vp, model; float color[4], opts[4]; } CogMeshCB;
static struct {
    cgltf_data *data;
    void *file;
    cgltf_skin *skin;
    CogM4 model, palette[64];
    CogMeshVertex *bind;
    int vertices;
    ID3D11Buffer *vb, *ib, *cb;
    ID3D11VertexShader *vs;
    ID3D11PixelShader *ps;
    ID3D11InputLayout *layout;
    ID3D11ShaderResourceView *tex[4];
    int textures, parts;
    struct { UINT first, count; float color[4], cutoff, emissive; int texture; } part[64];
    float rest[64][4], spring[64], springV[64];
} CogMesh;

static const char CogMeshShader[] =
"cbuffer Avatar : register(b0) { row_major float4x4 VP; row_major float4x4 Model; float4 Color; float4 Opt; };\n"
"Texture2D Atlas : register(t0); SamplerState Samp : register(s0);\n"
"struct VI { float3 p:POSITION; float3 n:NORMAL; float2 uv:TEXCOORD; uint4 j:BLENDINDICES; float4 w:BLENDWEIGHT; };\n"
"struct VO { float4 p:SV_Position; float3 n:NORMAL; float2 uv:TEXCOORD; };\n"
"VO vs_main(VI v) { VO o; o.p=mul(VP,mul(Model,float4(v.p,1)));o.n=mul((float3x3)Model,v.n);o.uv=v.uv;return o;}\n"
"float4 ps_main(VO v, bool front:SV_IsFrontFace):SV_Target { float4 c=Color; if(Opt.x>0)c*=Atlas.Sample(Samp,v.uv);if(c.a<Opt.y)discard;float3 n=normalize(v.n)*(front?1:-1);float diff=saturate(dot(n,normalize(float3(.35,.8,.6)))*.5+.5);float rim=pow(1-abs(n.z),3)*.08;return float4(c.rgb*(.55+.55*diff+Opt.z)+float3(.2,.08,.35)*rim,1);}\n";

static CogM4 CogMesh_Matrix(const float *col)
{
    CogM4 m;
    int r,c;
    for(r=0;r<4;r++)for(c=0;c<4;c++)m.m[r][c]=col[c*4+r];
    return m;
}

static int CogMesh_Joint(const char *name)
{
    int i;
    for(i=0;CogMesh.skin && i<(int)CogMesh.skin->joints_count;i++) {
        const char *n=CogMesh.skin->joints[i]->name;
        if(n && !SDL20_strcmp(n,name))return i;
    }
    return -1;
}

static void CogMesh_Rotate(int i,float x,float y,float z)
{
    cgltf_node *n;
    float cx,sx,cy,sy,cz,sz;
    if(i<0)return;
    n=CogMesh.skin->joints[i];
    cx=CogVR.cosf_(x*.5f);sx=CogVR.sinf_(x*.5f);cy=CogVR.cosf_(y*.5f);sy=CogVR.sinf_(y*.5f);cz=CogVR.cosf_(z*.5f);sz=CogVR.sinf_(z*.5f);
    n->has_rotation=1;n->has_matrix=0;
    n->rotation[0]=sx*cy*cz+cx*sy*sz;n->rotation[1]=cx*sy*cz-sx*cy*sz;
    n->rotation[2]=cx*cy*sz+sx*sy*cz;n->rotation[3]=cx*cy*cz-sx*sy*sz;
}

static CogV3 CogMesh_NodePos(cgltf_node *node)
{
    float m[16];cgltf_node_transform_world(node,m);return CogV3_Make(m[12],m[13],m[14]);
}

static void CogMesh_AimBone(cgltf_node *node,CogV3 bind,CogV3 wanted)
{
    float pm[16], dot,s;
    CogM4 parent,inv;
    CogV3 dir,axis;
    if(node->parent){cgltf_node_transform_world(node->parent,pm);parent=CogMesh_Matrix(pm);CogM4_RigidInverse(&inv,&parent);}
    else CogM4_Identity(&inv);
    dir=CogV3_Make(inv.m[0][0]*wanted.x+inv.m[0][1]*wanted.y+inv.m[0][2]*wanted.z,inv.m[1][0]*wanted.x+inv.m[1][1]*wanted.y+inv.m[1][2]*wanted.z,inv.m[2][0]*wanted.x+inv.m[2][1]*wanted.y+inv.m[2][2]*wanted.z);
    bind=CogV3_Norm(bind,CogV3_Make(0,-1,0));dir=CogV3_Norm(dir,bind);dot=CogVR_Clamp(CogV3_Dot(bind,dir),-1,1);
    axis=CogV3_Cross(bind,dir);s=CogVR.sqrtf_(SDL_max(.00001f,2*(1+dot)));
    node->has_rotation=1;node->has_matrix=0;
    if(dot<-.999f){node->rotation[0]=1;node->rotation[1]=node->rotation[2]=node->rotation[3]=0;}
    else {node->rotation[0]=axis.x/s;node->rotation[1]=axis.y/s;node->rotation[2]=axis.z/s;node->rotation[3]=s*.5f;}
}

static void CogMesh_Arm(const char *side,CogV3 target)
{
    char name[64];int a,b,c;cgltf_node *upper,*lower,*hand;CogV3 origin,dir,bend,elbow;float l1,l2,d,along,height;
    SDL20_snprintf(name,sizeof(name),"%sUpperArm",side);a=CogMesh_Joint(name);
    SDL20_snprintf(name,sizeof(name),"%sLowerArm",side);b=CogMesh_Joint(name);
    SDL20_snprintf(name,sizeof(name),"%sHand",side);c=CogMesh_Joint(name);
    if(a<0||b<0||c<0)return;
    upper=CogMesh.skin->joints[a];lower=CogMesh.skin->joints[b];hand=CogMesh.skin->joints[c];
    origin=CogMesh_NodePos(upper);dir=CogV3_Sub(target,origin);d=CogV3_Len(dir);
    l1=CogV3_Len(CogV3_Make(lower->translation[0],lower->translation[1],lower->translation[2]));
    l2=CogV3_Len(CogV3_Make(hand->translation[0],hand->translation[1],hand->translation[2]));
    if(l1<.001f||l2<.001f||d<.001f)return;
    dir=CogV3_Scale(dir,1/d);d=CogVR_Clamp(d,(l1>l2?l1-l2:l2-l1)+.001f,l1+l2-.001f);
    along=(l1*l1-l2*l2+d*d)/(2*d);height=CogVR.sqrtf_(SDL_max(0,l1*l1-along*along));
    bend=CogV3_Norm(CogV3_Sub(CogV3_Make(0,-1,-.2f),CogV3_Scale(dir,CogV3_Dot(dir,CogV3_Make(0,-1,-.2f)))),CogV3_Make(0,0,-1));
    elbow=CogV3_Add(origin,CogV3_Add(CogV3_Scale(dir,along),CogV3_Scale(bend,height)));
    CogMesh_AimBone(upper,CogV3_Make(lower->translation[0],lower->translation[1],lower->translation[2]),CogV3_Sub(elbow,origin));
    origin=CogMesh_NodePos(lower);
    CogMesh_AimBone(lower,CogV3_Make(hand->translation[0],hand->translation[1],hand->translation[2]),CogV3_Sub(target,origin));
}

static void CogMesh_Pose(const CogM4 *head,const CogM4 *body,float dt)
{
    CogM4 inv,b,ib;int i,h;float m[16],v[16],t=CogVR_Time();
    const int headId=CogMesh_Joint("head");
    CogMesh.model=*body;
    /* Asset +Z faces forward; the existing controller/avatar frame points -Z forward. */
    for(i=0;i<3;i++){CogMesh.model.m[i][0]=-body->m[i][0];CogMesh.model.m[i][2]=-body->m[i][2];}
    if(headId>=0){cgltf_node_transform_world(CogMesh.skin->joints[headId],m);for(i=0;i<3;i++)CogMesh.model.m[i][3]-=CogMesh.model.m[i][1]*m[13];}
    for(i=0;i<(int)CogMesh.skin->joints_count;i++)SDL20_memcpy(CogMesh.skin->joints[i]->rotation,CogMesh.rest[i],sizeof(CogMesh.rest[i]));
    CogMesh_Rotate(CogMesh_Joint("leftUpperLeg"),-.45f+.07f*CogVR.sinf_(t),0,-.10f);
    CogMesh_Rotate(CogMesh_Joint("leftLowerLeg"),.85f,0,0);
    CogMesh_Rotate(CogMesh_Joint("rightUpperLeg"),-.15f,0,.08f);
    CogMesh_Rotate(CogMesh_Joint("rightLowerLeg"),.5f+.04f*CogVR.sinf_(t+1),0,0);
    CogMesh_Rotate(CogMesh_Joint("leftFoot"),.15f,0,0);CogMesh_Rotate(CogMesh_Joint("rightFoot"),.2f,0,0);
    CogMesh_Rotate(CogMesh_Joint("chest"),.012f*CogVR.sinf_(t*1.4f),0,0);
    if(headId>=0) {
        const float horizontal=CogVR.sqrtf_(head->m[0][2]*head->m[0][2]+head->m[2][2]*head->m[2][2]);
        const float pitch=CogVR.atan2f_(-head->m[1][2],horizontal);
        const float roll=CogVR.atan2f_(head->m[1][0],head->m[1][1]);
        CogMesh_Rotate(headId,-pitch,0,roll);
    }
    CogMesh_Rotate(CogMesh_Joint("halo"),0,0.06f*CogVR.sinf_(t*.3f),0);
    for(i=0;i<(int)CogMesh.skin->joints_count;i++){
        const char *name=CogMesh.skin->joints[i]->name;
        if(name && (!SDL20_strncmp(name,"hair",4)||!SDL20_strncmp(name,"ahoge",5))){
            float left=SDL_min(dt,.1f),goal=CogVR_Clamp(CogVR.avVel.x*.003f,-.2f,.2f)+.015f*CogVR.sinf_(t*1.2f+i*.5f);
            while(left>0){const float step=SDL_min(left,.008f);CogMesh.springV[i]+=(35*(goal-CogMesh.spring[i])-10*CogMesh.springV[i])*step;CogMesh.spring[i]+=CogMesh.springV[i]*step;left-=step;}
            CogMesh_Rotate(i,0,0,CogMesh.spring[i]);
        }
    }
    CogM4_RigidInverse(&inv,&CogMesh.model);
    for(h=0;h<2;h++)if(CogVR.mode==2 && CogVR.hand[h].valid){
        const CogV3 p=CogVR.hand[h].pos;
        const CogV3 target=CogV3_Make(inv.m[0][0]*p.x+inv.m[0][1]*p.y+inv.m[0][2]*p.z+inv.m[0][3],inv.m[1][0]*p.x+inv.m[1][1]*p.y+inv.m[1][2]*p.z+inv.m[1][3],inv.m[2][0]*p.x+inv.m[2][1]*p.y+inv.m[2][2]*p.z+inv.m[2][3]);
        CogMesh_Arm(h?"right":"left",target);
    }
    for(i=0;i<(int)CogMesh.skin->joints_count;i++){
        cgltf_node_transform_world(CogMesh.skin->joints[i],m);b=CogMesh_Matrix(m);
        cgltf_accessor_read_float(CogMesh.skin->inverse_bind_matrices,i,v,16);ib=CogMesh_Matrix(v);
        CogM4_Mul(&CogMesh.palette[i],&b,&ib);
    }
    /* Once per frame, shared by all views. Wine's compiler cannot index a bone cbuffer dynamically. */
    {
        D3D11_MAPPED_SUBRESOURCE map;
        if (SUCCEEDED(ID3D11DeviceContext_Map(CogVR.ctx,(ID3D11Resource*)CogMesh.vb,0,D3D11_MAP_WRITE_DISCARD,0,&map))) {
            CogMeshVertex *dst=(CogMeshVertex*)map.pData;
            int vi,k,r;
            for(vi=0;vi<CogMesh.vertices;vi++) {
                const CogMeshVertex *src=&CogMesh.bind[vi];
                dst[vi]=*src;
                for(r=0;r<3;r++) { dst[vi].p[r]=0;dst[vi].n[r]=0; }
                for(k=0;k<4;k++) if(src->w[k]>0) {
                    const CogM4 *bm=&CogMesh.palette[src->j[k]];
                    for(r=0;r<3;r++) {
                        dst[vi].p[r]+=(bm->m[r][0]*src->p[0]+bm->m[r][1]*src->p[1]+bm->m[r][2]*src->p[2]+bm->m[r][3])*src->w[k];
                        dst[vi].n[r]+=(bm->m[r][0]*src->n[0]+bm->m[r][1]*src->n[1]+bm->m[r][2]*src->n[2])*src->w[k];
                    }
                }
            }
            ID3D11DeviceContext_Unmap(CogVR.ctx,(ID3D11Resource*)CogMesh.vb,0);
        }
    }
    (void)head;
}

/* world (tracking-space metres) matrix of the posed joint `name`; 0 without a loaded avatar or that joint */
static int CogMesh_JointWorld(const char *name,CogM4 *out)
{
    float m[16];CogM4 local;const int j=CogMesh_Joint(name);
    if(!CogMesh.vb||j<0)return 0;
    cgltf_node_transform_world(CogMesh.skin->joints[j],m);local=CogMesh_Matrix(m);
    CogM4_Mul(out,&CogMesh.model,&local);
    return 1;
}

static void CogMesh_Free(void)
{
    int i;COGVR_REL(CogMesh.vb);COGVR_REL(CogMesh.ib);COGVR_REL(CogMesh.cb);COGVR_REL(CogMesh.vs);COGVR_REL(CogMesh.ps);COGVR_REL(CogMesh.layout);
    for(i=0;i<4;i++)COGVR_REL(CogMesh.tex[i]);
    if(CogMesh.data) { cgltf_free(CogMesh.data); }
    SDL20_free(CogMesh.bind);SDL20_free(CogMesh.file);SDL20_zero(CogMesh);
}

/* Material splits often share geometry. Parse and skin each identical vertex set once. */
static int CogMesh_SameVertices(const cgltf_primitive *a,const cgltf_primitive *b)
{
    size_t i,j;
    if(a->attributes_count!=b->attributes_count)return 0;
    for(i=0;i<a->attributes_count;i++) {
        for(j=0;j<b->attributes_count;j++) {
            if(a->attributes[i].type==b->attributes[j].type && a->attributes[i].index==b->attributes[j].index && a->attributes[i].data==b->attributes[j].data)break;
        }
        if(j==b->attributes_count)return 0;
    }
    return 1;
}

static int CogMesh_Load(const char *path)
{
    HANDLE f;DWORD size,got;cgltf_options opts;cgltf_node *node=NULL;cgltf_mesh *mesh;int i,k,vi=0,ii=0,bases[64];size_t nv=0,ni=0;
    CogMeshVertex *verts=NULL;Uint32 *indices=NULL;D3D11_BUFFER_DESC bd;D3D11_SUBRESOURCE_DATA sd;ID3DBlob *vs=NULL,*ps=NULL,*err=NULL;pD3DCompile compile;HRESULT hr=E_FAIL;
    const D3D11_INPUT_ELEMENT_DESC layout[]={
        {"POSITION",0,DXGI_FORMAT_R32G32B32_FLOAT,0,0,D3D11_INPUT_PER_VERTEX_DATA,0},{"NORMAL",0,DXGI_FORMAT_R32G32B32_FLOAT,0,12,D3D11_INPUT_PER_VERTEX_DATA,0},
        {"TEXCOORD",0,DXGI_FORMAT_R32G32_FLOAT,0,24,D3D11_INPUT_PER_VERTEX_DATA,0},{"BLENDINDICES",0,DXGI_FORMAT_R16G16B16A16_UINT,0,32,D3D11_INPUT_PER_VERTEX_DATA,0},{"BLENDWEIGHT",0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,40,D3D11_INPUT_PER_VERTEX_DATA,0}};
    f=CreateFileA(path,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,0,NULL);if(f==INVALID_HANDLE_VALUE)return 0;
    size=GetFileSize(f,NULL);if(size<20||size>128*1024*1024){CloseHandle(f);return 0;}
    CogMesh.file=SDL20_malloc(size);if(!CogMesh.file){CloseHandle(f);return 0;}
    if(!ReadFile(f,CogMesh.file,size,&got,NULL)||got!=size){CloseHandle(f);goto fail;}CloseHandle(f);
    SDL20_zero(opts);
    if(cgltf_parse(&opts,CogMesh.file,size,&CogMesh.data)!=cgltf_result_success)goto fail;
    for(i=0;i<(int)CogMesh.data->buffers_count;i++)if(CogMesh.data->buffers[i].uri)goto fail;
    if(cgltf_load_buffers(&opts,CogMesh.data,NULL)!=cgltf_result_success||cgltf_validate(CogMesh.data)!=cgltf_result_success)goto fail;
    for(i=0;i<(int)CogMesh.data->nodes_count;i++)if(CogMesh.data->nodes[i].mesh&&CogMesh.data->nodes[i].skin){node=&CogMesh.data->nodes[i];break;}
    if(!node || node->skin->joints_count>64 || !node->skin->inverse_bind_matrices)goto fail;
    CogMesh.skin=node->skin;mesh=node->mesh;if(mesh->primitives_count>64||CogMesh.data->images_count>4)goto fail;
    for(i=0;i<(int)mesh->primitives_count;i++){
        cgltf_primitive *p=&mesh->primitives[i];
        int previous;
        if(p->type!=cgltf_primitive_type_triangles||!p->indices)goto fail;
        for(previous=0;previous<i;previous++)if(CogMesh_SameVertices(p,&mesh->primitives[previous]))break;
        if(previous<i) { bases[i]=bases[previous]; }
        else {
            bases[i]=(int)nv;
            for(k=0;k<(int)p->attributes_count;k++)if(p->attributes[k].type==cgltf_attribute_type_position)nv+=p->attributes[k].data->count;
        }
        ni+=p->indices->count;
    }
    if(!nv||nv>200000||ni>600000) { goto fail; }
    verts=SDL20_calloc(nv,sizeof(*verts));indices=SDL20_malloc(ni*sizeof(*indices));if(!verts||!indices)goto fail;
    for(i=0;i<(int)mesh->primitives_count;i++){
        cgltf_primitive *p=&mesh->primitives[i];cgltf_accessor *pos=NULL,*normal=NULL,*uv=NULL,*joint=NULL,*weight=NULL;size_t j;cgltf_material *mat=p->material;
        for(k=0;k<(int)p->attributes_count;k++){cgltf_attribute *a=&p->attributes[k];switch(a->type){case cgltf_attribute_type_position:pos=a->data;break;case cgltf_attribute_type_normal:normal=a->data;break;case cgltf_attribute_type_texcoord:if(a->index==0)uv=a->data;break;case cgltf_attribute_type_joints:joint=a->data;break;case cgltf_attribute_type_weights:weight=a->data;break;default:break;}}
        if(!pos||!normal||!joint||!weight)goto fail;
        if(bases[i]==vi) {
            for(j=0;j<pos->count;j++){cgltf_uint ids[4];CogMeshVertex *v=&verts[vi+j];cgltf_accessor_read_float(pos,j,v->p,3);cgltf_accessor_read_float(normal,j,v->n,3);if(uv)cgltf_accessor_read_float(uv,j,v->uv,2);cgltf_accessor_read_uint(joint,j,ids,4);cgltf_accessor_read_float(weight,j,v->w,4);for(k=0;k<4;k++){if(ids[k]>=CogMesh.skin->joints_count)goto fail;v->j[k]=(Uint16)ids[k];}}
            vi+=pos->count;
        }
        CogMesh.part[i].first=ii;CogMesh.part[i].count=p->indices->count;CogMesh.part[i].texture=-1;
        for(j=0;j<p->indices->count;j++){const size_t ix=cgltf_accessor_read_index(p->indices,j);if(ix>=pos->count)goto fail;indices[ii++]=(Uint32)(bases[i]+ix);}
        for(k=0;k<4;k++)CogMesh.part[i].color[k]=mat?mat->pbr_metallic_roughness.base_color_factor[k]:1;
        if(mat){cgltf_texture *tex=mat->pbr_metallic_roughness.base_color_texture.texture;if(tex&&tex->image)CogMesh.part[i].texture=(int)(tex->image-CogMesh.data->images);CogMesh.part[i].cutoff=mat->alpha_mode==cgltf_alpha_mode_mask?mat->alpha_cutoff:0;CogMesh.part[i].emissive=(mat->emissive_factor[0]+mat->emissive_factor[1]+mat->emissive_factor[2])/3;}
    }
    for(i=0;i<(int)CogMesh.data->images_count;i++){
        cgltf_image *image=&CogMesh.data->images[i];int w,h,n;unsigned char *px;ID3D11Texture2D *tex=NULL;D3D11_TEXTURE2D_DESC td;
        if(!image->buffer_view) { goto fail; }
        px=stbi_load_from_memory(cgltf_buffer_view_data(image->buffer_view),(int)image->buffer_view->size,&w,&h,&n,4);if(!px)goto fail;
        if(w>4096||h>4096){stbi_image_free(px);goto fail;}SDL20_zero(td);td.Width=w;td.Height=h;td.MipLevels=td.ArraySize=1;td.Format=DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;td.SampleDesc.Count=1;td.Usage=D3D11_USAGE_IMMUTABLE;td.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        SDL20_zero(sd);sd.pSysMem=px;sd.SysMemPitch=w*4;hr=ID3D11Device_CreateTexture2D(CogVR.dev,&td,&sd,&tex);stbi_image_free(px);if(FAILED(hr))goto fail;
        hr=ID3D11Device_CreateShaderResourceView(CogVR.dev,(ID3D11Resource*)tex,NULL,&CogMesh.tex[i]);ID3D11Texture2D_Release(tex);if(FAILED(hr))goto fail;
    }
    SDL20_zero(bd);bd.Usage=D3D11_USAGE_DYNAMIC;bd.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;bd.BindFlags=D3D11_BIND_VERTEX_BUFFER;bd.ByteWidth=nv*sizeof(*verts);SDL20_zero(sd);sd.pSysMem=verts;if(FAILED(ID3D11Device_CreateBuffer(CogVR.dev,&bd,&sd,&CogMesh.vb)))goto fail;
    bd.Usage=D3D11_USAGE_IMMUTABLE;bd.CPUAccessFlags=0;bd.BindFlags=D3D11_BIND_INDEX_BUFFER;bd.ByteWidth=ni*sizeof(*indices);sd.pSysMem=indices;if(FAILED(ID3D11Device_CreateBuffer(CogVR.dev,&bd,&sd,&CogMesh.ib)))goto fail;
    bd.Usage=D3D11_USAGE_DYNAMIC;bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;bd.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;bd.ByteWidth=sizeof(CogMeshCB);if(FAILED(ID3D11Device_CreateBuffer(CogVR.dev,&bd,NULL,&CogMesh.cb)))goto fail;
    compile=(pD3DCompile)(void*)GetProcAddress(CogVR.hComp,"D3DCompile");if(!compile)goto fail;
    hr=compile(CogMeshShader,sizeof(CogMeshShader)-1,"avatar.hlsl",NULL,NULL,"vs_main","vs_4_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&vs,&err);if(FAILED(hr))goto fail;COGVR_REL(err);
    hr=compile(CogMeshShader,sizeof(CogMeshShader)-1,"avatar.hlsl",NULL,NULL,"ps_main","ps_4_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&ps,&err);if(FAILED(hr))goto fail;
    if(FAILED(ID3D11Device_CreateVertexShader(CogVR.dev,ID3D10Blob_GetBufferPointer(vs),ID3D10Blob_GetBufferSize(vs),NULL,&CogMesh.vs)))goto fail;
    if(FAILED(ID3D11Device_CreatePixelShader(CogVR.dev,ID3D10Blob_GetBufferPointer(ps),ID3D10Blob_GetBufferSize(ps),NULL,&CogMesh.ps)))goto fail;
    if(FAILED(ID3D11Device_CreateInputLayout(CogVR.dev,layout,5,ID3D10Blob_GetBufferPointer(vs),ID3D10Blob_GetBufferSize(vs),&CogMesh.layout)))goto fail;
    for(i=0;i<(int)CogMesh.skin->joints_count;i++){cgltf_node *n=CogMesh.skin->joints[i];SDL20_memcpy(CogMesh.rest[i],n->rotation,sizeof(CogMesh.rest[i]));if(!n->has_rotation)CogMesh.rest[i][3]=1;}
    CogMesh.parts=mesh->primitives_count;CogMesh.textures=CogMesh.data->images_count;CogMesh.bind=verts;CogMesh.vertices=nv;SDL20_free(indices);COGVR_REL(vs);COGVR_REL(ps);COGVR_REL(err);
    CogVR_Log("COGVR: GLB avatar loaded: %u vertices, %u triangles, %u joints, %d materials",(unsigned)nv,(unsigned)(ni/3),(unsigned)CogMesh.skin->joints_count,CogMesh.parts);return 1;
fail:
    if(err)CogVR_Log("COGVR: avatar shader: %s",(const char*)ID3D10Blob_GetBufferPointer(err));
    CogVR_Log("COGVR: GLB avatar rejected: %s",path);SDL20_free(verts);SDL20_free(indices);COGVR_REL(vs);COGVR_REL(ps);COGVR_REL(err);CogMesh_Free();return 0;
}

static void CogMesh_Draw(const CogM4 *vp,int eyePass)
{
    const UINT stride=sizeof(CogMeshVertex),offset=0;int i;
    if(!CogMesh.vb||eyePass)return;
    ID3D11DeviceContext_IASetInputLayout(CogVR.ctx,CogMesh.layout);ID3D11DeviceContext_IASetVertexBuffers(CogVR.ctx,0,1,&CogMesh.vb,&stride,&offset);ID3D11DeviceContext_IASetIndexBuffer(CogVR.ctx,CogMesh.ib,DXGI_FORMAT_R32_UINT,0);
    ID3D11DeviceContext_VSSetShader(CogVR.ctx,CogMesh.vs,NULL,0);ID3D11DeviceContext_PSSetShader(CogVR.ctx,CogMesh.ps,NULL,0);ID3D11DeviceContext_VSSetConstantBuffers(CogVR.ctx,0,1,&CogMesh.cb);ID3D11DeviceContext_PSSetConstantBuffers(CogVR.ctx,0,1,&CogMesh.cb);
    for(i=0;i<CogMesh.parts;i++){
        D3D11_MAPPED_SUBRESOURCE map;ID3D11ShaderResourceView *srv=CogMesh.part[i].texture>=0?CogMesh.tex[CogMesh.part[i].texture]:NULL;
        if(SUCCEEDED(ID3D11DeviceContext_Map(CogVR.ctx,(ID3D11Resource*)CogMesh.cb,0,D3D11_MAP_WRITE_DISCARD,0,&map))){CogMeshCB *cb=map.pData;cb->vp=*vp;cb->model=CogMesh.model;SDL20_memcpy(cb->color,CogMesh.part[i].color,sizeof(cb->color));cb->opts[0]=srv?1:0;cb->opts[1]=CogMesh.part[i].cutoff;cb->opts[2]=CogMesh.part[i].emissive;cb->opts[3]=0;ID3D11DeviceContext_Unmap(CogVR.ctx,(ID3D11Resource*)CogMesh.cb,0);}
        ID3D11DeviceContext_PSSetShaderResources(CogVR.ctx,0,1,&srv);ID3D11DeviceContext_DrawIndexed(CogVR.ctx,CogMesh.part[i].count,CogMesh.part[i].first,0);
    }
    {ID3D11ShaderResourceView *none=NULL;ID3D11DeviceContext_PSSetShaderResources(CogVR.ctx,0,1,&none);}
}
#endif
