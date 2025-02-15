/*-----------------------------------------------------------------------------
Copyright(c) 2010 - 2018 ViSUS L.L.C.,
Scientific Computing and Imaging Institute of the University of Utah

ViSUS L.L.C., 50 W.Broadway, Ste. 300, 84101 - 2044 Salt Lake City, UT
University of Utah, 72 S Central Campus Dr, Room 3750, 84112 Salt Lake City, UT

All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met :

* Redistributions of source code must retain the above copyright notice, this
list of conditions and the following disclaimer.

* Redistributions in binary form must reproduce the above copyright notice,
this list of conditions and the following disclaimer in the documentation
and/or other materials provided with the distribution.

* Neither the name of the copyright holder nor the names of its
contributors may be used to endorse or promote products derived from
this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED.IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

For additional information about this project contact : pascucci@acm.org
For support : support@visus.net
-----------------------------------------------------------------------------*/

#include <Visus/RenderArrayNode.h>
#include <Visus/GLCanvas.h>
#include <Visus/StringTree.h>
#include <Visus/GLPhongShader.h>

#if VISUS_OSPRAY
#if WIN32
#pragma warning(disable:4005)
#endif
#include <ospray/ospray.h>
#include <ospray/ospray_cpp.h>
using namespace ospray;
using namespace ospcommon;
#endif

namespace Visus {

/////////////////////////////////////////////////////////////////////////////
class RenderArrayNode::Pimpl
{
public:

  //destructor
  virtual ~Pimpl() {}

  //setData
  virtual void setData(Array data, SharedPtr<Palette> palette) = 0;

  //glRender
  virtual void glRender(GLCanvas& gl) = 0;

};


/////////////////////////////////////////////////////////////////////////////
class OpenGLRenderArrayNode : public RenderArrayNode::Pimpl
{
public:

  //___________________________________________________________________________
  class Config
  {
  public:

    int  texture_dim = 0;       //2 or 3
    int  texture_nchannels = 0; //(1) luminance (2) luminance+alpha (3) rgb (4) rgba
    bool clippingbox_enabled = false;
    bool palette_enabled = false;
    bool lighting_enabled = false;
    bool discard_if_zero_alpha = false;

    //constructor
    Config() {}

    //valid
    bool valid() const {
      return (texture_dim == 2 || texture_dim == 3) && (texture_nchannels >= 1 && texture_nchannels <= 4);
    }

    //operator<
    bool operator<(const Config& other) const {
      return this->key() < other.key();
    }

  private:

    //key
    std::tuple<int, int, bool, bool, bool, bool> key() const {
      VisusAssert(valid());
      return std::make_tuple(texture_dim, texture_nchannels, clippingbox_enabled, palette_enabled, lighting_enabled, discard_if_zero_alpha);
    }

  };

  //___________________________________________________________________________
  class MyShader : public GLShader
  {
  public:

    VISUS_NON_COPYABLE_CLASS(MyShader)

    Config config;

    GLSampler u_sampler;
    GLSampler u_palette_sampler;
    GLUniform u_opacity;

    //constructor
    MyShader(const Config& config_) :
      GLShader(":/RenderArrayShader.glsl"),
      config(config_)
    {
      addDefine("CLIPPINGBOX_ENABLED", cstring(config.clippingbox_enabled ? 1 : 0));
      addDefine("TEXTURE_DIM", cstring(config.texture_dim));
      addDefine("TEXTURE_NCHANNELS", cstring(config.texture_nchannels));
      addDefine("LIGHTING_ENABLED", cstring(config.lighting_enabled ? 1 : 0));
      addDefine("PALETTE_ENABLED", cstring(config.palette_enabled ? 1 : 0));
      addDefine("DISCARD_IF_ZERO_ALPHA", cstring(config.discard_if_zero_alpha ? 1 : 0));

      u_sampler = addSampler("u_sampler");
      u_palette_sampler = addSampler("u_palette_sampler");
      u_opacity = addUniform("u_opacity");
    }

    //destructor
    virtual ~MyShader() {
    }

    //shaders
    static std::map<Config, SharedPtr<MyShader> >& shaders() {
      static std::map<Config, SharedPtr<MyShader> > ret;
      return ret;
    }

    //getSingleton
    static MyShader* getSingleton(const Config& config) {
      auto it = shaders().find(config);
      if (it != shaders().end()) return it->second.get();
      auto ret = std::make_shared<MyShader>(config);
      shaders()[config] = ret;
      return ret.get();
    }

    //setTexture
    void setTexture(GLCanvas& gl, SharedPtr<GLTexture> value) {
      gl.setTexture(u_sampler, value);
    }

    //setPaletteTexture 
    void setPaletteTexture(GLCanvas& gl, SharedPtr<GLTexture> value)
    {
      VisusAssert(config.palette_enabled);
      gl.setTextureInSlot(1, u_palette_sampler, value);
    }

    //setOpacity
    void setOpacity(GLCanvas& gl, double value) {
      gl.setUniform(u_opacity, (float)value);
    }

  };

  RenderArrayNode* owner;
  Array data;
  SharedPtr<GLTexture> data_texture;
  SharedPtr<GLTexture> palette_texture;

  //constructor
  OpenGLRenderArrayNode(RenderArrayNode* owner_) : owner(owner_) {
  }

  //destructor
  virtual ~OpenGLRenderArrayNode() {
  }

  //setData
  virtual void setData(Array data, SharedPtr<Palette> palette) override
  {
    VisusAssert(VisusHasMessageLock());

    auto failed = [&]() {
      this->data = Array();
      this->data_texture.reset();
      this->palette_texture.reset();
    };

    Time t1 = Time::now();

    if (!data.valid() || !data.dims.innerProduct() || !data.dtype.valid())
      return failed();

    //not supported
    if (data.dtype.ncomponents() >= 5)
      return failed();
  
    bool got_new_data = (data.heap != this->data.heap);

    //if I fail to create the texture, keep the current data
    SharedPtr<GLTexture> data_texture=this->data_texture;
    if (got_new_data || !data_texture)
    {
      data_texture = GLTexture::createFromArray(data);
      if (!data_texture)
        return;
    }

    //if I fail to create the texture, keep the current data
    SharedPtr<GLTexture> palette_texture;
    if (palette)
    {
      auto array = palette->toArray();
      //ArrayUtils::saveImage("tmp.png", array);
      palette_texture = GLTexture::createFromArray(array);
      if (!palette_texture)
        return;
    }

    this->data = data;
    this->data_texture = data_texture;
    this->palette_texture = palette_texture;

    //overrule filter
    this->data_texture->minfilter = owner->minifyFilter();
    this->data_texture->magfilter = owner->magnifyFilter();

    //how to map texture value to [0,1] range
    {
      auto& ranges = this->data_texture->ranges;

      int N = data.dtype.ncomponents();
      for (int C = 0; C < std::min(4, N); C++)
      {
        ranges[C] = Palette::ComputeRange(data, C,
          /*bNormalizeToFloat*/true,
          palette ? palette->getNormalizationMode() : TransferFunction::FieldRange,
          palette ? palette->getUserRange() : Range::invalid());
      }

      //1 component will end up in texture RGB, I want all channels to be the same (as it was gray)
      if (N == 1)
      {
        ranges = std::vector<Range>(3, this->data_texture->ranges[0]);
        ranges.push_back(Range(0.0, 1.0, 0.0));
      }
    }

    //if you want to see what's going on...
#if 0
    {
      static int cont = 0;
      ArrayUtils::saveImageUINT8(concatenate("tmp/debug_render_array_node/", cont++, ".png"), *data);
    }
#endif

    //note I'm not sure if the texture can be generated... 
    PrintInfo("got array",
      "dims", data.dims, "dtype", data.dtype,
      "scheduling texture upload", data_texture->dims, data_texture->dtype, StringUtils::getStringFromByteSize(data_texture->dtype.getByteSize(this->data_texture->dims)),
      "got_new_data", got_new_data,
      "msec", t1.elapsedMsec());
  }

  //glRender
  virtual void glRender(GLCanvas& gl) override 
  {
    if (!data_texture)
      return;

    if (!data_texture->uploaded())
    {
      if (!data_texture->textureId(gl))
      {
        PrintInfo("Failed to upload the texture");
        return;
      }

      PrintInfo("Uploaded texture", data_texture->dims, data_texture->dtype, StringUtils::getStringFromByteSize(data_texture->dtype.getByteSize(this->data_texture->dims)));
    }

    //need to upload a new palette?
    bool b3D = data.dims.getPointDim() > 2 &&
      data.dims[0] > 1 &&
      data.dims[1] > 1 &&
      data.dims[2] > 1;

    Config config;
    config.texture_dim = b3D ? 3 : 2;
    config.texture_nchannels = data.dtype.ncomponents();
    config.palette_enabled = palette_texture ? true : false;
    config.lighting_enabled = owner->lightingEnabled();
    config.clippingbox_enabled = gl.hasClippingBox() || (data.clipping.valid() || owner->useViewDirection());
    config.discard_if_zero_alpha = true;

    MyShader* shader = MyShader::getSingleton(config);
    gl.setShader(shader);

    if (shader->config.lighting_enabled)
    {
      gl.setUniformMaterial(*shader, owner->getLightingMaterial());

      Point3d pos, dir, vup;
      gl.getModelview().getLookAt(pos, dir, vup);
      gl.setUniformLight(*shader, Point4d(pos, 1.0));
    }

    if (data.clipping.valid())
      gl.pushClippingBox(data.clipping);

    //in 3d I render the box (0,0,0)x(1,1,1)
    //in 2d I render the box (0,0)  x(1,1)
    gl.pushModelview();
    {
      auto box = data.bounds.getBoxNd();

      gl.multModelview(data.bounds.getTransformation());
      gl.multModelview(Matrix::translate(box.p1));
      gl.multModelview(Matrix::nonZeroScale(box.size()));
      if (shader->config.texture_dim == 2)
      {
        if (!box.size()[0])
          gl.multModelview(Matrix(Point3d(0, 1, 0), Point3d(0, 0, 1), Point3d(1, 0, 0), Point3d(0, 0, 0)));

        else if (!box.size()[1])
          gl.multModelview(Matrix(Point3d(1, 0, 0), Point3d(0, 0, 1), Point3d(0, 1, 0), Point3d(0, 0, 0)));
      }
    }

    gl.pushBlend(true);
    gl.pushDepthMask(shader->config.texture_dim == 3 ? false : true);

    //note: the order seeems to be important, first bind GL_TEXTURE1 then GL_TEXTURE0
    if (shader->config.palette_enabled && palette_texture)
      shader->setPaletteTexture(gl, palette_texture);

    shader->setTexture(gl, data_texture);

    if (b3D)
    {
      double opacity = owner->opacity;

      int nslices = owner->maxNumSlices();
      if (nslices <= 0)
      {
        double factor = owner->useViewDirection() ? 2.5 : 1; //show a little more if use_view_direction!
        nslices = (int)(*data.dims.max_element() * factor);
      }

      //see https://github.com/sci-visus/visus/issues/99
      const int max_nslices = 128;
      if (owner->bFastRendering && nslices > max_nslices) {
        opacity *= nslices / (double)max_nslices;
        nslices = max_nslices;
      }

      shader->setOpacity(gl, opacity);

      if (owner->useViewDirection())
      {
        if (!data.clipping.valid())
          gl.pushClippingBox(BoxNd(Point3d(0, 0, 0), Point3d(1, 1, 1)));

        gl.glRenderMesh(GLMesh::ViewDependentUnitVolume(gl.getFrustum(), nslices));

        if (!data.clipping.valid())
          gl.popClippingBox();
      }
      else
        gl.glRenderMesh(GLMesh::AxisAlignedUnitVolume(gl.getFrustum(), nslices));
    }
    else
    {
      shader->setOpacity(gl, owner->opacity);
      gl.glRenderMesh(GLMesh::Quad(Point2d(0, 0), Point2d(1, 1),/*bNormal*/shader->config.lighting_enabled,/*bTexCoord*/true));
    }

    gl.popDepthMask();
    gl.popBlend();
    gl.popModelview();

    if (data.clipping.valid())
      gl.popClippingBox();
  }
};


/////////////////////////////////////////////////////////////////////////////////////
#if VISUS_OSPRAY
class OSPRayRenderArrayNode : public RenderArrayNode::Pimpl{
public:

  RenderArrayNode* owner;

  // Note: The C++ wrappers automatically manage life time tracking and reference
  // counting for the OSPRay objects, and OSPRay internally tracks references to
  // parameters
  ospray::cpp::Instance instance;
  ospray::cpp::World world;
  ospray::cpp::Camera camera;
  ospray::cpp::Renderer renderer;
  ospray::cpp::FrameBuffer framebuffer;

  Frustum prev_frustum;
  bool sceneChanged = true;
  bool volumeValid = false;

  float varianceThreshold = 15.f;

  const size_t npaletteSamples = 128;

  //constructor
  OSPRayRenderArrayNode(RenderArrayNode* owner_) : owner(owner_)
  {
    world = cpp::World();
    camera = cpp::Camera("perspective");

    // TODO: Need a way to tell visus to keep calling render even if the camera isn't moving
    // so we can do progressive refinement
    if (VisusModule::getModuleConfig()->readString("Configuration/VisusViewer/DefaultRenderNode/ospray_renderer") == "pathtracer") 
      renderer = cpp::Renderer("pathtracer");
    else 
      renderer = cpp::Renderer("scivis");

    // create and setup an ambient light
    cpp::Light ambient_light("ambient");
    ambient_light.setParam("intensity", 0.25f);
    ambient_light.commit();

    cpp::Light directional_light("distant");
    directional_light.setParam("direction", math::vec3f(0.5f, 1.f, 0.25f));
    directional_light.setParam("intensity", 10.f);
    directional_light.commit();
    std::vector<cpp::Light> lights = { ambient_light, directional_light };
    world.setParam("light", cpp::Data(lights));

    // This is the Colors::DarkBlue color but pre-transformed srgb -> linear
    // so that when it's converted to srgb for display it will match the
    // regular render node (which doesn't seem to do srgb?)
    const math::vec3f bgColor(0.021219f, 0.0423114f, 0.093059f);
    renderer.setParam("backgroundColor", bgColor);
    renderer.setParam("varianceThreshold", varianceThreshold);
    renderer.setParam("maxPathLength", int(8));
    renderer.setParam("volumeSamplingRate", 0.25f);
    renderer.commit();
  }

  //destructor
  virtual ~OSPRayRenderArrayNode() {
  }

  //convertData (OSPRay shares the data pointer with us, does not copy internally)
  cpp::Data convertData(Array array) const
  {
    auto W = array.getWidth();
    auto H = array.getHeight();
    auto D = array.getDepth();

    const OSPDataType ospray_dtype = DTypeToOSPDtype(array.dtype);

    if (ospray_dtype == OSP_UCHAR)
      return cpp::Data(math::vec3ul(W, H, D), reinterpret_cast<uint8_t*>(array.c_ptr()), true);

    if (ospray_dtype == OSP_USHORT)
      return cpp::Data(math::vec3ul(W, H, D), reinterpret_cast<uint16_t*>(array.c_ptr()), true);

    if (ospray_dtype == OSP_FLOAT)
      return cpp::Data(math::vec3ul(W, H, D), reinterpret_cast<float*>(array.c_ptr()), true);

    if (ospray_dtype == OSP_DOUBLE)
      return cpp::Data(math::vec3ul(W, H, D), reinterpret_cast<double*>(array.c_ptr()), true);

    PrintInfo("OSPRay only supports scalar voxel types");
    return cpp::Data();
  }

  //convertPalette
  cpp::TransferFunction convertPalette(Array data, SharedPtr<Palette> palette)
  {
    auto value_range = Palette::ComputeRange(data, /*component*/0,
      /*bNormalizeToFloat*/false,
      palette ? palette->getNormalizationMode() : Palette::FieldRange,
      palette ? palette->getUserRange() : Range::invalid());

    std::vector<math::vec3f> tfnColors(npaletteSamples, math::vec3f(0.f));
    std::vector<float> tfnOpacities(npaletteSamples, 0.f);

    // Assumes functions = {R, G, B, A}
    for (size_t i = 0; i < npaletteSamples; ++i)
    {
      const float x = static_cast<float>(i) / npaletteSamples;
      tfnColors[i][0] = palette->R->getValue(x);
      tfnColors[i][1] = palette->G->getValue(x);
      tfnColors[i][2] = palette->B->getValue(x);
      tfnOpacities[i] = palette->A->getValue(x);
    }

    cpp::TransferFunction ret("piecewiseLinear");
    ret.setParam("color", cpp::Data(tfnColors));
    ret.setParam("opacity", cpp::Data(tfnOpacities));
    ret.setParam("valueRange", math::vec2f(value_range.from, value_range.to));
    ret.commit();
    return ret;
  }

  //setData
  virtual void setData(Array data, SharedPtr<Palette> palette) override
  {
    if (!data.valid() || data.getPointDim() != 3)
    {
      this->volumeValid = false;
      return;
    }
   
    this->sceneChanged = true;

    cpp::Volume volume = cpp::Volume("structuredRegular");
    auto ospray_data = convertData(data);
    this->volumeValid = ospray_data.type ? true : false;
    if (!this->volumeValid)
      return;
    volume.setParam("data", ospray_data);

    // Scale the smaller volumes we get while loading progressively to fill the true bounds of the full dataset
    //size of the grid cells in object-space
    auto bbox = data.bounds.toAxisAlignedBox().withPointDim(3);
    volume.setParam("gridOrigin", math::vec3f(bbox.p1[0], bbox.p1[1], bbox.p1[2]));
    volume.setParam("gridSpacing", math::vec3f(
      bbox.size()[0] / data.getWidth(), 
      bbox.size()[1] / data.getHeight(), 
      bbox.size()[2] / data.getDepth()));
    volume.commit();

    cpp::VolumetricModel volume_with_palette(volume);
    volume_with_palette.setParam("transferFunction", convertPalette(data, palette));
    volume_with_palette.commit();

    cpp::Group group;
    group.setParam("volume", cpp::Data(volume_with_palette));
    group.commit();

    cpp::Instance instance(group);
    instance.commit();
    world.setParam("instance", cpp::Data(instance));

    // TODO some lights?
    // ...

    world.commit();

    if (data.clipping.valid())
      PrintInfo("CLIPPING TODO");
  }

  //refreshCameraIfNeeded
  bool refreshCameraIfNeeded(Viewport viewport, Matrix projection,Matrix modelview)
  {
    bool bSomethingChanged = false;

    if (prev_frustum.getModelview() != modelview)
    {
      // Extract camera parameters from model view matrix
      // TODO track camera position to see if it changed and reset accum only if that changed

      Point3d pos, dir, vup;
      modelview.getLookAt(pos, dir, vup);
      this->camera.setParam("position", math::vec3f(pos.x, pos.y, pos.z));
      this->camera.setParam("direction", math::vec3f(dir.x, dir.y, dir.z));
      this->camera.setParam("up", math::vec3f(vup.x, vup.y, vup.z));
      bSomethingChanged = true;
    }

    if (prev_frustum.getProjection() != projection)
    {
      //see https://stackoverflow.com/questions/56428880/how-to-extract-camera-parameters-from-projection-matrix
      float fov = Utils::radiantToDegree(2.0 * atan(1.0f / projection(1, 1)));
      float aspect = projection(1, 1) / projection(0, 0);
      auto znear = projection(3, 2) / (projection(2, 2) - 1.0);
      auto zfar = projection(3, 2) / (projection(2, 2) + 1.0);
      this->camera.setParam("nearClip", znear);
      this->camera.setParam("farClip", zfar);
      this->camera.setParam("fovy", fov);
      this->camera.setParam("aspect", aspect);
      this->camera.setParam("apertureRadius", 0.0f);
      bSomethingChanged = true;
    }

    // change in the viewport
    if (prev_frustum.getViewport() != viewport)
    {
      // On windows it seems like the ref-counting doesn't quite work and we leak?
      ospRelease(framebuffer.handle());
      framebuffer = cpp::FrameBuffer(math::vec2i(viewport.width, viewport.height), OSP_FB_SRGBA, OSP_FB_COLOR | OSP_FB_ACCUM | OSP_FB_VARIANCE);
      bSomethingChanged = true;
    }

    this->prev_frustum = Frustum(viewport, projection, modelview);
    
    if (bSomethingChanged)
      camera.commit();
    
    return bSomethingChanged;
  }

  //glRender
  virtual void glRender(GLCanvas& gl) override
  {
    if (!volumeValid) {
      PrintInfo("Skipping rendering unsupported volume");
      return;
    }

    this->sceneChanged = refreshCameraIfNeeded(gl.getViewport(),gl.getProjection(),gl.getModelview());
    if (this->sceneChanged)
      framebuffer.clear();
    this->sceneChanged = false;

    framebuffer.renderFrame(renderer, camera, world);

    // Blit the rendered framebuffer from OSPRay
    {
      auto W = gl.getViewport().width;
      auto H = gl.getViewport().height;
      uint32_t* fb = (uint32_t*)framebuffer.map(OSP_FB_COLOR);
      auto fbArray = Array(W, H, DTypes::UINT8_RGBA, HeapMemory::createUnmanaged((Uint8*)fb, W * H * 4));
      auto fbTexture = GLTexture::createFromArray(fbArray);
      if (!fbTexture)
        return;

      fbTexture->minfilter = GL_NEAREST;
      fbTexture->magfilter = GL_NEAREST;

      GLPhongShader* shader = GLPhongShader::getSingleton(GLPhongShader::Config().withTextureEnabled(true));
      gl.setShader(shader);
      shader->setUniformColor(gl, Colors::White);
      shader->setTexture(gl, fbTexture);

      // Render directly to normalized device coordinates and overwrite everything
      // with the rendered result from OSPRay
      gl.pushFrustum();
      gl.setHud();
      gl.pushDepthTest(false);
      gl.pushBlend(true);
      gl.glRenderMesh(GLMesh::Quad(Point2d(0, 0), Point2d(W, H), /*bNormal*/false, /*bTextCoord*/true));
      gl.popBlend();
      gl.popDepthTest();
      gl.popFrustum();
      framebuffer.unmap(fb);
    }
    
    if (ospGetVariance(framebuffer.handle()) > varianceThreshold) 
      gl.postRedisplay();
  }

  //DTypeToOSPDtype
  static OSPDataType DTypeToOSPDtype(const DType& dtype) {

    if (dtype == DTypes::INT8) return OSP_CHAR;

    if (dtype == DTypes::UINT8) return OSP_UCHAR;
    if (dtype == DTypes::UINT8_GA) return OSP_VEC2UC;
    if (dtype == DTypes::UINT8_RGB) return OSP_VEC3UC;
    if (dtype == DTypes::UINT8_RGBA) return OSP_VEC4UC;

    if (dtype == DTypes::INT16) return OSP_SHORT;
    if (dtype == DTypes::UINT16) return OSP_USHORT;

    if (dtype == DTypes::INT32) return OSP_INT;
    if (dtype == DTypes::UINT32) return OSP_UINT;

    if (dtype == DTypes::INT64) return OSP_LONG;
    if (dtype == DTypes::UINT64) return OSP_ULONG;

    if (dtype == DTypes::FLOAT32) return OSP_FLOAT;
    if (dtype == DTypes::FLOAT32_GA) return OSP_VEC2F;
    if (dtype == DTypes::FLOAT32_RGB) return OSP_VEC3F;
    if (dtype == DTypes::FLOAT32_RGBA) return OSP_VEC4F;

    if (dtype == DTypes::FLOAT64) return OSP_DOUBLE;

    return OSP_UNKNOWN;
  }

  //OspDTypeStr
  static String OspDTypeStr(const OSPDataType t)
  {
    switch (t)
    {
    case OSP_UCHAR:  return "uchar";
    case OSP_SHORT:  return "short";
    case OSP_USHORT: return "ushort";
    case OSP_FLOAT: return  "float";
    case OSP_DOUBLE: return "double";
    default: break;
    }
    ThrowException("Unsupported data type for OSPVolume");
    return nullptr;
  }

};

#endif // #if VISUS_OSPRAY

/////////////////////////////////////////////////////////////////////////////////////////////////////////
RenderArrayNode::RenderArrayNode()
{
  addInputPort("array");
  addInputPort("palette");

  this->render_type = "OpenGL";
  this->pimpl = new OpenGLRenderArrayNode(this);

  //this->render_type = "OSPRay";
  //this->pimpl = new OSPRayRenderArrayNode(this);

  lighting_material.front.ambient=Colors::Black;
  lighting_material.front.diffuse=Colors::White;
  lighting_material.front.specular=Colors::White;
  lighting_material.front.shininess=100;

  lighting_material.back.ambient=Colors::Black;
  lighting_material.back.diffuse=Colors::White;
  lighting_material.back.specular=Colors::White;
  lighting_material.back.shininess=100;
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////
RenderArrayNode::~RenderArrayNode()
{
  if (pimpl) {
    delete pimpl;
    pimpl = nullptr;
  }
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////
void RenderArrayNode::execute(Archive& ar)
{
  if (ar.name == "SetLightingMaterial") {
    GLMaterial value;
    value.read(*ar.getFirstChild());
    setLightingMaterial(value);
    return;
  }

  if (ar.name == "SetLightingEnabled") {
    bool value;
    ar.read("value", value);
    setLightingEnabled(value);
    return;
  }

  if (ar.name == "SetPaletteEnabled") {
    bool value;
    ar.read("value", value);
    setPaletteEnabled(value);
    return;
  }

  if (ar.name == "SetUseViewDirection") {
    bool value;
    ar.read("value", value);
    setUseViewDirection(value);
    return;
  }

  if (ar.name == "SetMaxNumSlices") {
    int value;
    ar.read("value", value);
    setMaxNumSlices(value);
    return;
  }

  if (ar.name == "SetMinifyFilter") {
    int value;
    ar.read("value", value);
    setMinifyFilter(value);
    return;
  }

  if (ar.name == "SetMagnifyFilter") {
    int value;
    ar.read("value", value);
    setMagnifyFilter(value);
    return;
  }

  if (ar.name == "SetRenderType") {
    String value;
    ar.read("value", value);
    setRenderType(value);
    return;
  }

  return Node::execute(ar);
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////
void RenderArrayNode::setData(Array data,SharedPtr<Palette> palette)
{
  VisusAssert(VisusHasMessageLock());

  //invalid data?
  if (!data.valid() || !data.dims.innerProduct() || !data.dtype.valid())
  {
    data = Array();
    palette.reset();
  }

  this->data    = data;
  this->palette = palette;
  pimpl->setData(data, palette);
}


/////////////////////////////////////////////////////////////////////////////////////////////////////////
bool RenderArrayNode::processInput()
{
  //I want to sign the input return receipt only after the rendering
  auto return_receipt = createPassThroughtReceipt();
  auto palette        = readValue<Palette>("palette");
  auto data           = readValue<Array>("array");

  this->return_receipt.reset();

  if (!data || !data->dims.innerProduct() || !data->dtype.valid())
  {
    setData(Array(), SharedPtr<Palette>());
    return false;
  }

  //so far I can apply the transfer function on the GPU only if the data is atomic
  //TODO: i can support even 3 and 4 component arrays
  bool bPaletteEnabled = paletteEnabled() || (palette && data->dtype.ncomponents() == 1);
  if (!bPaletteEnabled)
    palette.reset();

  this->return_receipt = return_receipt; //wait until the
  setData(*data, palette);

  return true;
}


/////////////////////////////////////////////////////////////////////////////////////////////////////////
void RenderArrayNode::setRenderType(String value)
{
  if (value.empty())
    value = "OpenGL";

#if !VISUS_OSPRAY
  value = "OpenGL";
#endif

  //useless call
  if (value == render_type)
    return;

  //deallocate old pimpl
  if (pimpl) {
    delete pimpl;
    pimpl = nullptr;
  }

#if VISUS_OSPRAY
  if (value == "OSPRay")
    pimpl = new OSPRayRenderArrayNode(this);
#endif

  if (!pimpl)
  {
    value = "OpenGL";
    this->pimpl = new OpenGLRenderArrayNode(this);
  }
  
  //set new pimpl
  pimpl->setData(this->data, this->palette);
  setProperty("SetRenderType", this->render_type, value);
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////
void RenderArrayNode::glRender(GLCanvas& gl) 
{
  auto return_receipt = this->return_receipt;
  this->return_receipt.reset();

  if (!data.valid())
    return;

  pimpl->glRender(gl);
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////
void RenderArrayNode::write(Archive& ar) const
{
  Node::write(ar);
  ar.write("lighting_enabled", lighting_enabled);
  ar.write("palette_enabled", palette_enabled);
  ar.write("use_view_direction", use_view_direction);
  ar.write("max_num_slices", max_num_slices);
  ar.write("magnify_filter", magnify_filter);
  ar.write("minify_filter", minify_filter);
  ar.write("render_type", render_type);
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////
void RenderArrayNode::read(Archive& ar)
{
  Node::read(ar);
  ar.read("lighting_enabled", lighting_enabled);
  ar.read("palette_enabled", palette_enabled);
  ar.read("use_view_direction", use_view_direction);
  ar.read("max_num_slices", max_num_slices);
  ar.read("magnify_filter", magnify_filter);
  ar.read("minify_filter", minify_filter);

  //need to create the pimpl
  {
    String value;
    ar.read("render_type", value);
    setRenderType(value);
  }
}

void RenderArrayNode::allocShaders()
{
  //nothing to do
}

void RenderArrayNode::releaseShaders()
{
  OpenGLRenderArrayNode::MyShader::shaders().clear();
}

 
/////////////////////////////////////////////////////////
class RenderArrayNodeView :
  public QFrame,
  public View<RenderArrayNode>
{
public:

  //constructor
  RenderArrayNodeView(RenderArrayNode* model = nullptr) {
    if (model)
      bindModel(model);
  }

  //destructor
  virtual ~RenderArrayNodeView() {
    bindModel(nullptr);
  }

  //bindModel
  virtual void bindModel(RenderArrayNode* model) override
  {
    if (this->model)
    {
      QUtils::clearQWidget(this);
    }

    View<ModelClass>::bindModel(model);

    if (this->model)
    {
      std::map< int, String> filter_options = {
        {GL_LINEAR,"linear"},
        {GL_NEAREST,"nearest"}
      };

      QFormLayout* layout = new QFormLayout();
      layout->addRow("Enable lighting", GuiFactory::CreateCheckBox(model->lightingEnabled(), "", [model](int value) {model->setLightingEnabled(value); }));
      layout->addRow("Minify filter", GuiFactory::CreateIntegerComboBoxWidget(model->minifyFilter(), filter_options, [model](int value) {model->setMinifyFilter(value); }));
      layout->addRow("Magnify filter", GuiFactory::CreateIntegerComboBoxWidget(model->magnifyFilter(), filter_options, [model](int value) {model->setMagnifyFilter(value); }));
      layout->addRow("Enable Palette", GuiFactory::CreateCheckBox(model->paletteEnabled(), "", [model](int value) {model->setPaletteEnabled(value); }));
      layout->addRow("Use view direction", GuiFactory::CreateCheckBox(model->useViewDirection(), "", [model](int value) {model->setUseViewDirection(value); }));
      layout->addRow("Max slices", GuiFactory::CreateIntegerTextBoxWidget(model->maxNumSlices(), [model](int value) {model->setMaxNumSlices(value); }));

#if VISUS_OSPRAY
      std::vector<String> options = { "OpenGL" , "OSPRay" };
#else
      std::vector<String> options = { "OpenGL" };
#endif

      layout->addRow("Render dtype",GuiFactory::CreateComboBox(model->getRenderType().c_str(), options, [model](String s) { model->setRenderType(s); }));

      setLayout(layout);
    }
  }

};

void RenderArrayNode::createEditor()
{
  auto win = new RenderArrayNodeView(this);
  win->show();
}

} //namespace Visus

