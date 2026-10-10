/* Transform feedback of a legacy (GLSL 1.20) program fed by immediate mode:
   gl_Position and the built-in varying gl_FrontColor are captured from a
   glBegin/glEnd triangle (compatibility profile, W22). */
#include <windows.h>
#include <GL/gl.h>
#include <stdint.h>
#include <stdio.h>
#define TF 0x8c8e
#define STATIC_DRAW 0x88e4
#define VERTEX_SHADER 0x8b31
#define INTERLEAVED 0x8c8c
#define LINK_STATUS 0x8b82
typedef char GLchar_;
typedef void(WINAPI*Gen)(GLsizei,GLuint*);typedef void(WINAPI*Bind)(GLenum,GLuint);typedef void(WINAPI*Data)(GLenum,intptr_t,const void*,GLenum);typedef void(WINAPI*Base)(GLenum,GLuint,GLuint);typedef void(WINAPI*BeginTF)(GLenum);typedef void(WINAPI*EndTF)(void);typedef void(WINAPI*Get)(GLenum,intptr_t,intptr_t,void*);
typedef GLuint(WINAPI*CreateShader)(GLenum);typedef void(WINAPI*ShaderSource)(GLuint,GLsizei,const GLchar_*const*,const GLint*);typedef void(WINAPI*CompileShader)(GLuint);typedef GLuint(WINAPI*CreateProgram)(void);typedef void(WINAPI*AttachShader)(GLuint,GLuint);typedef void(WINAPI*LinkProgram)(GLuint);typedef void(WINAPI*UseProgram)(GLuint);typedef void(WINAPI*GetProgramiv)(GLuint,GLenum,GLint*);typedef void(WINAPI*Varyings)(GLuint,GLsizei,const GLchar_*const*,GLenum);
static void*p(const char*n){PROC q=wglGetProcAddress(n);HMODULE m;if(q)return(void*)q;m=GetModuleHandleA("opengl32.dll");return m?(void*)GetProcAddress(m,n):0;}
#define L(t,v,n)do{v=(t)p(n);if(!v){printf("FAIL missing %s\n",n);return 14;}}while(0)
static const GLchar_*vs="#version 120\nvoid main(){gl_Position=gl_Vertex;gl_FrontColor=gl_Color;}\n";
int main(void){PIXELFORMATDESCRIPTOR d={0};HWND w;HDC dc;HGLRC c;int pf;Gen gen;Bind bind;Data data;Base base;BeginTF begin;EndTF end;Get get;CreateShader createShader;ShaderSource shaderSource;CompileShader compileShader;CreateProgram createProgram;AttachShader attachShader;LinkProgram linkProgram;UseProgram useProgram;GetProgramiv getProgramiv;Varyings varyings;GLuint b,s,prog;GLint linked=0;float out[24]={0};const GLchar_*names[2]={"gl_Position","gl_FrontColor"};
w=CreateWindowA("STATIC","WineMetalGL transform",WS_POPUP,0,0,64,64,0,0,0,0);if(!w)return 11;dc=GetDC(w);d.nSize=sizeof(d);d.nVersion=1;d.dwFlags=PFD_DRAW_TO_WINDOW|PFD_SUPPORT_OPENGL;d.iPixelType=PFD_TYPE_RGBA;d.cColorBits=32;pf=ChoosePixelFormat(dc,&d);if(!pf||!SetPixelFormat(dc,pf,&d))return 12;c=wglCreateContext(dc);if(!c||!wglMakeCurrent(dc,c))return 13;
L(Gen,gen,"glGenBuffers");L(Bind,bind,"glBindBuffer");L(Data,data,"glBufferData");L(Base,base,"glBindBufferBase");L(BeginTF,begin,"glBeginTransformFeedback");L(EndTF,end,"glEndTransformFeedback");L(Get,get,"glGetBufferSubData");
L(CreateShader,createShader,"glCreateShader");L(ShaderSource,shaderSource,"glShaderSource");L(CompileShader,compileShader,"glCompileShader");L(CreateProgram,createProgram,"glCreateProgram");L(AttachShader,attachShader,"glAttachShader");L(LinkProgram,linkProgram,"glLinkProgram");L(UseProgram,useProgram,"glUseProgram");L(GetProgramiv,getProgramiv,"glGetProgramiv");L(Varyings,varyings,"glTransformFeedbackVaryings");
s=createShader(VERTEX_SHADER);shaderSource(s,1,&vs,0);compileShader(s);prog=createProgram();attachShader(prog,s);varyings(prog,2,names,INTERLEAVED);linkProgram(prog);getProgramiv(prog,LINK_STATUS,&linked);if(!linked){printf("FAIL link\n");return 16;}useProgram(prog);
gen(1,&b);bind(TF,b);data(TF,sizeof(out),0,STATIC_DRAW);base(TF,0,b);glEnable(0x8c89/*RASTERIZER_DISCARD*/);begin(GL_TRIANGLES);glBegin(GL_TRIANGLES);glColor4f(1,0,0,1);glVertex3f(-1,-1,0);glColor4f(0,1,0,1);glVertex3f(3,-1,0);glVertex3f(-1,3,0);glEnd();end();glDisable(0x8c89);get(TF,0,sizeof(out),out);
printf("TF v0 position=%g,%g,%g,%g color=%g,%g,%g,%g v1 color=%g,%g,%g,%g error=0x%x\n",out[0],out[1],out[2],out[3],out[4],out[5],out[6],out[7],out[12],out[13],out[14],out[15],(unsigned)glGetError());
if(out[0]!=-1||out[1]!=-1||out[3]!=1||out[4]!=1||out[5]!=0||out[8]!=3||out[12]!=0||out[13]!=1)return 15;printf("WINEMETALGL_TRANSFORM_FIXED_OK\n");wglMakeCurrent(0,0);wglDeleteContext(c);ReleaseDC(w,dc);DestroyWindow(w);return 0;}
