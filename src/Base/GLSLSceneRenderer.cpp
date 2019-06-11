/*!
  @file
  @author Shin'ichiro Nakaoka
*/

#include "GLSLSceneRenderer.h"
#include "ShaderPrograms.h"
#include <cnoid/SceneDrawables>
#include <cnoid/SceneCameras>
#include <cnoid/SceneLights>
#include <cnoid/SceneEffects>
#include <cnoid/EigenUtil>
#include <cnoid/NullOut>
#include <Eigen/StdVector>
#include <GL/glu.h>
#include <boost/optional.hpp>
#include <unordered_map>
#include <mutex>
#include <iostream>
#include <stdexcept>

using namespace std;
using namespace cnoid;

namespace {

const bool USE_FBO_FOR_PICKING = true;
const bool SHOW_IMAGE_FOR_PICKING = false;
const bool USE_GL_INT_2_10_10_10_REV_FOR_NORMALS = true;
const bool USE_GL_SHORT_FOR_VERTICES = false;
const bool USE_GL_HALF_FLOAT_FOR_TEXTURE_COORDINATES = false;
const bool USE_GL_UNSIGNED_SHORT_FOR_TEXTURE_COORDINATES = false;

const float MinLineWidthForPicking = 5.0f;

typedef vector<Affine3, Eigen::aligned_allocator<Affine3>> Affine3Array;

std::mutex extensionMutex;
set<GLSLSceneRenderer*> renderers;
vector<std::function<void(GLSLSceneRenderer* renderer)>> extendFunctions;

const bool LOCK_VERTEX_ARRAY_API_TO_AVOID_CRASH_ON_NVIDIA_LINUX_OPENGL_DRIVER = true;

std::mutex vertexArrayMutex;

struct LockVertexArrayAPI
{
    LockVertexArrayAPI(){
        if(LOCK_VERTEX_ARRAY_API_TO_AVOID_CRASH_ON_NVIDIA_LINUX_OPENGL_DRIVER){
            vertexArrayMutex.lock();
        }
    }
    ~LockVertexArrayAPI(){
        if(LOCK_VERTEX_ARRAY_API_TO_AVOID_CRASH_ON_NVIDIA_LINUX_OPENGL_DRIVER){
            vertexArrayMutex.unlock();
        }
    }
};
        
class GLResource : public Referenced
{
public:
    virtual void discard() = 0;
};

typedef ref_ptr<GLResource> GLResourcePtr;

class VertexResource : public GLResource
{
public:
    EIGEN_MAKE_ALIGNED_OPERATOR_NEW
    
    static const int MAX_NUM_BUFFERS = 4;
    GLuint vao;
    GLuint vbos[MAX_NUM_BUFFERS];
    GLsizei numVertices;
    int numBuffers;
    SgObjectPtr sceneObject;
    ScopedConnection connection;
    SgLineSetPtr normalVisualization;
    Matrix4* pLocalTransform;
    Matrix4 localTransform;
    
    VertexResource(const VertexResource&) = delete;
    VertexResource& operator=(const VertexResource&) = delete;

    VertexResource(GLSLSceneRendererImpl* renderer, SgObject* obj)
        : sceneObject(obj)
    {
        connection.reset(
            obj->sigUpdated().connect(
                [&](const SgUpdate&){ numVertices = 0; }));

        clearHandles();
        glGenVertexArrays(1, &vao);
        pLocalTransform = nullptr;
    }

    void clearHandles(){
        vao = 0;
        for(int i=0; i < MAX_NUM_BUFFERS; ++i){
            vbos[i] = 0;
        }
        numBuffers = 0;
        numVertices = 0;
    }

    virtual void discard() override { clearHandles(); }

    bool isValid(){
        if(numVertices > 0){
            return true;
        } else if(numBuffers > 0){
            deleteBuffers();
        }
        return false;
    }

    GLuint newBuffer(){
        GLuint buffer;
        glGenBuffers(1, &buffer);
        vbos[numBuffers++] = buffer;
        return buffer;
    }

    void deleteBuffers(){
        if(numBuffers > 0){
            glDeleteBuffers(numBuffers, vbos);
            for(int i=0; i < numBuffers; ++i){
                vbos[i] = 0;
            }
            numBuffers = 0;
        }
    }

    GLuint vbo(int index) {
        return vbos[index];
    }

    ~VertexResource() {
        deleteBuffers();
        if(vao > 0){
            glDeleteVertexArrays(1, &vao);
        }
    }
};

typedef ref_ptr<VertexResource> VertexResourcePtr;

class TextureResource : public GLResource
{
public:
    bool isLoaded;
    bool isImageUpdateNeeded;
    GLuint textureId;
    GLuint samplerId;
    int width;
    int height;
    int numComponents;
        
    TextureResource(){
        isLoaded = false;
        isImageUpdateNeeded = false;
        textureId = 0;
        samplerId = 0;
        width = 0;
        height = 0;
        numComponents = 0;
    }

    ~TextureResource(){
        clear();
    }

    virtual void discard() override { isLoaded = false; }

    void clear() {
        if(isLoaded){
            if(textureId){
                glDeleteTextures(1, &textureId);
                textureId = 0;
            }
            if(samplerId){
                glDeleteSamplers(1, &samplerId);
                samplerId = 0;
            }
            isLoaded = false;
        }
    }
    
    bool isSameSizeAs(const Image& image){
        return (width == image.width() && height == image.height() && numComponents == image.numComponents());
    }
};

typedef ref_ptr<TextureResource> TextureResourcePtr;

struct SgObjectPtrHash {
    std::hash<SgObject*> hash;
    std::size_t operator()(const SgObjectPtr& p) const {
        return hash(p.get());
    }
};

typedef std::unordered_map<SgObjectPtr, GLResourcePtr, SgObjectPtrHash> GLResourceMap;

}

namespace cnoid {

class GLSLSceneRendererImpl
{
public:
    EIGEN_MAKE_ALIGNED_OPERATOR_NEW
        
    GLSLSceneRenderer* self;

    PolymorphicFunctionSet<SgNode> renderingFunctions;

    GLuint defaultFBO;
    GLuint fboForPicking;
    GLuint colorBufferForPicking;
    GLuint depthBufferForPicking;
    int viewportWidth;
    int viewportHeight;
    bool needToChangeBufferSizeForPicking;

    ShaderProgram* currentProgram;
    LightingProgram* currentLightingProgram;
    NolightingProgram* currentNolightingProgram;

    NolightingProgram nolightingProgram;
    SolidColorProgram solidColorProgram;
    MinimumLightingProgram minimumLightingProgram;
    PhongShadowLightingProgram phongShadowLightingProgram;

    struct ProgramInfo {
        ShaderProgram* program;
        LightingProgram* lightingProgram;
        NolightingProgram* nolightingProgram;
    };
    vector<ProgramInfo> programStack;

    bool isActuallyRendering;
    bool isPicking;
    bool isRenderingShadowMap;
    bool isMinimumLightingProgramActivatedInThisFrame;
    
    Affine3Array modelMatrixStack; // stack of the model matrices
    Affine3 viewMatrix;
    Matrix4 projectionMatrix;
    Matrix4 PV;

    vector<function<void()>> postRenderingFunctions;
    vector<function<void()>> transparentRenderingFunctions;

    std::set<int> shadowLightIndices;

    int lightingMode;
    SgMaterialPtr defaultMaterial;
    GLfloat defaultPointSize;
    GLfloat defaultLineWidth;
    
    GLResourceMap resourceMaps[2];
    GLResourceMap* currentResourceMap;
    GLResourceMap* nextResourceMap;
    int currentResourceMapIndex;
    bool doUnusedResourceCheck;
    bool isCheckingUnusedResources;
    bool hasValidNextResourceMap;
    bool isResourceClearRequested;

    vector<char> scaledImageBuf;

    bool isCurrentFogUpdated;
    SgFogPtr prevFog;
    ScopedConnection currentFogConnection;

    bool defaultSmoothShading;
    bool isNormalVisualizationEnabled;
    float normalVisualizationLength;
    SgMaterialPtr normalVisualizationMaterial;

    // OpenGL states
    enum StateFlag {
        CULL_FACE,
        POINT_SIZE,
        LINE_WIDTH,
        NUM_STATE_FLAGS
    };
    vector<bool> stateFlag;

    int backFaceCullingMode;
    bool isCullFaceEnabled;

    float pointSize;
    float lineWidth;

    GLdouble pickX;
    GLdouble pickY;
    typedef std::shared_ptr<SgNodePath> SgNodePathPtr;
    SgNodePath currentNodePath;
    vector<SgNodePathPtr> pickingNodePathList;
    SgNodePath pickedNodePath;
    Vector3 pickedPoint;

    ostream* os_;
    ostream& os() { return *os_; }

    bool isUpsideDownEnabled;

    std::mutex newExtensionMutex;
    vector<std::function<void(GLSLSceneRenderer* renderer)>> newExtendFunctions;

    void renderChildNodes(SgGroup* group){
        for(auto p = group->cbegin(); p != group->cend(); ++p){
            renderingFunctions.dispatch(*p);
        }
    }
    
    GLSLSceneRendererImpl(GLSLSceneRenderer* self);
    ~GLSLSceneRendererImpl();
    void initialize();
    void onExtensionAdded(std::function<void(GLSLSceneRenderer* renderer)> func);
    void updateDefaultFramebufferObject();
    bool initializeGL();
    void doRender();
    bool doPick(int x, int y);
    void renderScene();
    bool renderShadowMap(int lightIndex);
    void beginRendering();
    void renderCamera(SgCamera* camera, const Affine3& cameraPosition);
    void renderLights(LightingProgram* program);
    void renderFog(LightingProgram* program);
    void endRendering();
    void renderSceneGraphNodes();
    void pushProgram(ShaderProgram& program, bool isLightingProgram);
    void popProgram();
    inline void setPickColor(unsigned int id);
    inline unsigned int pushPickId(SgNode* node, bool doSetColor = true);
    void popPickId();
    void renderGroup(SgGroup* group);
    void renderTransform(SgTransform* transform);
    void renderSwitch(SgSwitch* node);
    void renderUnpickableGroup(SgUnpickableGroup* group);
    void renderShape(SgShape* shape);
    void renderShapeMain(SgShape* shape, const Affine3& position, unsigned int pickId);
    void renderPointSet(SgPointSet* pointSet);        
    void renderLineSet(SgLineSet* lineSet);        
    void renderOverlay(SgOverlay* overlay);
    void renderOutlineGroup(SgOutlineGroup* outline);
    void renderOutlineGroupMain(SgOutlineGroup* outline, const Affine3& T);
    void renderSimplifiedRenderingGroup(SgSimplifiedRenderingGroup* group);
    void flushNolightingTransformMatrices();
    VertexResource* getOrCreateVertexResource(SgObject* obj);
    void drawVertexResource(VertexResource* resource, GLenum primitiveMode, const Affine3& position);
    void renderTransparentObjects();
    void renderMaterial(const SgMaterial* material);
    bool renderTexture(SgTexture* texture);
    bool loadTextureImage(TextureResource* resource, const Image& image);
    void writeMeshVertices(SgMesh* mesh, VertexResource* resource, SgTexture* texResource);
    template<class NormalArrayWrapper>
    bool writeMeshNormalsSub(SgMesh* mesh, NormalArrayWrapper& normals, VertexResource* resource);
    void writeMeshNormalsPacked(SgMesh* mesh, GLuint buffer, VertexResource* resource);
    void writeMeshNormalsFloat(SgMesh* mesh, GLuint buffer, VertexResource* resource);
    template<typename value_type, GLenum gltype, GLboolean normalized, class TexCoordArrayWrapper>
    void writeMeshTexCoordsSub(SgMesh* mesh, GLuint buffer, SgTexture* texture, TexCoordArrayWrapper& texCoords);
    void writeMeshTexCoordsHalfFloat(SgMesh* mesh, GLuint buffer, SgTexture* texture);
    void writeMeshTexCoordsUnsignedShort(SgMesh* mesh, GLuint buffer, SgTexture* texture);
    void writeMeshTexCoordsFloat(SgMesh* mesh, GLuint buffer, SgTexture* texture);
    void writeMeshColors(SgMesh* mesh, GLuint buffer);
    void renderPlot(SgPlot* plot, GLenum primitiveMode, std::function<SgVertexArrayPtr()> getVertices);
    void clearGLState();
    void setPointSize(float size);
    void setLineWidth(float width);
    void getCurrentCameraTransform(Affine3& T);
};

}


GLSLSceneRenderer::GLSLSceneRenderer()
{
    impl = new GLSLSceneRendererImpl(this);
    impl->initialize();
}


GLSLSceneRenderer::GLSLSceneRenderer(SgGroup* sceneRoot)
    : GLSceneRenderer(sceneRoot)
{
    impl = new GLSLSceneRendererImpl(this);
    impl->initialize();
}


GLSLSceneRendererImpl::GLSLSceneRendererImpl(GLSLSceneRenderer* self)
    : self(self)
{

}


void GLSLSceneRendererImpl::initialize()
{
    {
        std::lock_guard<std::mutex> guard(extensionMutex);
        renderers.insert(self);
    }
    
    defaultFBO = 0;
    fboForPicking = 0;
    colorBufferForPicking = 0;
    depthBufferForPicking = 0;
    viewportWidth = 1;
    viewportHeight = 1;
    needToChangeBufferSizeForPicking = true;

    currentProgram = nullptr;
    currentLightingProgram = nullptr;
    currentNolightingProgram = nullptr;

    isActuallyRendering = false;
    isPicking = false;
    isRenderingShadowMap = false;
    pickedPoint.setZero();

    doUnusedResourceCheck = true;
    currentResourceMapIndex = 0;
    hasValidNextResourceMap = false;
    isResourceClearRequested = false;
    currentResourceMap = &resourceMaps[0];
    nextResourceMap = &resourceMaps[1];

    backFaceCullingMode = GLSceneRenderer::ENABLE_BACK_FACE_CULLING;

    modelMatrixStack.reserve(16);
    viewMatrix.setIdentity();
    projectionMatrix.setIdentity();

    lightingMode = GLSceneRenderer::FULL_LIGHTING;
    defaultSmoothShading = true;
    defaultMaterial = new SgMaterial;
    defaultMaterial->setDiffuseColor(Vector3f(0.8, 0.8, 0.8));
    defaultPointSize = 1.0f;
    defaultLineWidth = 1.0f;

    isNormalVisualizationEnabled = false;
    normalVisualizationLength = 0.0f;
    normalVisualizationMaterial = new SgMaterial;
    normalVisualizationMaterial->setDiffuseColor(Vector3f(0.0f, 1.0f, 0.0f));

    isUpsideDownEnabled = false;

    stateFlag.resize(NUM_STATE_FLAGS, false);
    clearGLState();

    os_ = &nullout();

    renderingFunctions.setFunction<SgGroup>(
        [&](SgGroup* node){ renderGroup(node); });
    renderingFunctions.setFunction<SgTransform>(
        [&](SgTransform* node){ renderTransform(node); });
    renderingFunctions.setFunction<SgSwitch>(
        [&](SgSwitch* node){ renderSwitch(node); });
    renderingFunctions.setFunction<SgUnpickableGroup>(
        [&](SgUnpickableGroup* node){ renderUnpickableGroup(node); });
    renderingFunctions.setFunction<SgShape>(
        [&](SgShape* node){ renderShape(node); });
    renderingFunctions.setFunction<SgPointSet>(
        [&](SgPointSet* node){ renderPointSet(node); });
    renderingFunctions.setFunction<SgLineSet>(
        [&](SgLineSet* node){ renderLineSet(node); });
    renderingFunctions.setFunction<SgOverlay>(
        [&](SgOverlay* node){ renderOverlay(node); });
    renderingFunctions.setFunction<SgOutlineGroup>(
        [&](SgOutlineGroup* node){ renderOutlineGroup(node); });
    renderingFunctions.setFunction<SgSimplifiedRenderingGroup>(
        [&](SgSimplifiedRenderingGroup* node){ renderSimplifiedRenderingGroup(node); });

    self->applyExtensions();
    renderingFunctions.updateDispatchTable();
}


GLSLSceneRenderer::~GLSLSceneRenderer()
{
    std::lock_guard<std::mutex> guard(extensionMutex);
    renderers.erase(this);
    
    delete impl;
}


GLSLSceneRendererImpl::~GLSLSceneRendererImpl()
{
    // clear handles to avoid the deletion of them without the corresponding GL context
    for(int i=0; i < 2; ++i){
        GLResourceMap& resourceMap = resourceMaps[i];
        for(GLResourceMap::iterator p = resourceMap.begin(); p != resourceMap.end(); ++p){
            GLResource* resource = p->second;
            resource->discard();
        }
    }

    if(fboForPicking){
        glDeleteRenderbuffers(1, &colorBufferForPicking);
        glDeleteRenderbuffers(1, &depthBufferForPicking);
        glDeleteFramebuffers(1, &fboForPicking);
    }
}


void GLSLSceneRenderer::addExtension(std::function<void(GLSLSceneRenderer* renderer)> func)
{
    {
        std::lock_guard<std::mutex> guard(extensionMutex);
        extendFunctions.push_back(func);
    }
    for(GLSLSceneRenderer* renderer : renderers){
        renderer->impl->onExtensionAdded(func);
    }
}


void GLSLSceneRenderer::applyExtensions()
{
    SceneRenderer::applyExtensions();
    
    std::lock_guard<std::mutex> guard(extensionMutex);
    for(size_t i=0; i < extendFunctions.size(); ++i){
        extendFunctions[i](this);
    }
}


void GLSLSceneRendererImpl::onExtensionAdded(std::function<void(GLSLSceneRenderer* renderer)> func)
{
    std::lock_guard<std::mutex> guard(newExtensionMutex);
    newExtendFunctions.push_back(func);
}


bool GLSLSceneRenderer::applyNewExtensions()
{
    bool applied = SceneRenderer::applyNewExtensions();
    
    std::lock_guard<std::mutex> guard(impl->newExtensionMutex);
    if(!impl->newExtendFunctions.empty()){
        for(size_t i=0; i < impl->newExtendFunctions.size(); ++i){
            impl->newExtendFunctions[i](this);
        }
        impl->newExtendFunctions.clear();
        applied = true;
    }

    return applied;
}


SceneRenderer::NodeFunctionSet* GLSLSceneRenderer::renderingFunctions()
{
    return &impl->renderingFunctions;
}


void GLSLSceneRenderer::setOutputStream(std::ostream& os)
{
    GLSceneRenderer::setOutputStream(os);
    impl->os_ = &os;
}


void GLSLSceneRendererImpl::updateDefaultFramebufferObject()
{
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, reinterpret_cast<GLint*>(&defaultFBO));
    phongShadowLightingProgram.setDefaultFramebufferObject(defaultFBO);
}


bool GLSLSceneRenderer::initializeGL()
{
    GLSceneRenderer::initializeGL();
    return impl->initializeGL();
}


bool GLSLSceneRendererImpl::initializeGL()
{
    if(ogl_LoadFunctions() == ogl_LOAD_FAILED){
        return false;
    }

    updateDefaultFramebufferObject();

    try {
        nolightingProgram.initialize();
        solidColorProgram.initialize();
        minimumLightingProgram.initialize();
        phongShadowLightingProgram.initialize();
    }
    catch(std::runtime_error& error){
        os() << error.what() << endl;
        cout << error.what() << endl;
        return false;
    }

    glEnable(GL_DEPTH_TEST);
    glDisable(GL_DITHER);

    glEnable(GL_VERTEX_PROGRAM_POINT_SIZE);

    isResourceClearRequested = true;

    isCurrentFogUpdated = false;

    return true;
}


void GLSLSceneRenderer::flush()
{
    glFlush();

    /**
       This is necessary when the rendering is done for an internal frame buffer object
       and the rendererd image data is retrieved from it because another frame buffer object
       may be bounded in the renderer.
    */
    glBindFramebuffer(GL_FRAMEBUFFER, impl->defaultFBO);
}


void GLSLSceneRenderer::setViewport(int x, int y, int width, int height)
{
    GLSceneRenderer::setViewport(x, y, width, height);
    impl->viewportWidth = width;
    impl->viewportHeight = height;
    impl->needToChangeBufferSizeForPicking = true;
}


void GLSLSceneRenderer::requestToClearResources()
{
    impl->isResourceClearRequested = true;
}


void GLSLSceneRenderer::doRender()
{
    impl->doRender();
}


void GLSLSceneRendererImpl::doRender()
{
    updateDefaultFramebufferObject();
    
    if(self->applyNewExtensions()){
        renderingFunctions.updateDispatchTable();
    }

    self->extractPreprocessedNodes();
    beginRendering();

    isMinimumLightingProgramActivatedInThisFrame = false;
    
    if(lightingMode == GLSceneRenderer::NO_LIGHTING){
        pushProgram(nolightingProgram, false);
        isMinimumLightingProgramActivatedInThisFrame = true;
        
    } else if(lightingMode == GLSceneRenderer::SOLID_COLOR_LIGHTING){
        pushProgram(solidColorProgram, false);
        
    } else if(lightingMode == GLSceneRenderer::MINIMUM_LIGHTING){
        pushProgram(minimumLightingProgram, true);

    } else { // FULL_LIGHTING
        auto& program = phongShadowLightingProgram;

        if(shadowLightIndices.empty()){
            program.setNumShadows(0);
        } else {
            Array4i vp = self->viewport();
            int w, h;
            program.getShadowMapSize(w, h);
            self->setViewport(0, 0, w, h);
            pushProgram(program.shadowMapProgram(), false);
            isRenderingShadowMap = true;
            isActuallyRendering = false;
        
            int shadowMapIndex = 0;
            set<int>::iterator iter = shadowLightIndices.begin();
            while(iter != shadowLightIndices.end() && shadowMapIndex < program.maxNumShadows()){
                program.activateShadowMapGenerationPass(shadowMapIndex);
                int shadowLightIndex = *iter;
                if(renderShadowMap(shadowLightIndex)){
                    ++shadowMapIndex;
                }
                ++iter;
            }
            program.setNumShadows(shadowMapIndex);
        
            popProgram();
            isRenderingShadowMap = false;
            self->setViewport(vp[0], vp[1], vp[2], vp[3]);
        }
    
        program.activateMainRenderingPass();
        pushProgram(program, true);
    }
    
    isActuallyRendering = true;
    const Vector3f& c = self->backgroundColor();
    glClearColor(c[0], c[1], c[2], 1.0f);

    switch(self->polygonMode()){
    case GLSceneRenderer::FILL_MODE:
        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
        break;
    case GLSceneRenderer::LINE_MODE:
        glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
        break;
    case GLSceneRenderer::POINT_MODE:
        glPolygonMode(GL_FRONT_AND_BACK, GL_POINT);
        break;
    }
    
    renderScene();

    popProgram();
    endRendering();
}


bool GLSLSceneRenderer::doPick(int x, int y)
{
    return impl->doPick(x, y);
}


bool GLSLSceneRendererImpl::doPick(int x, int y)
{
    if(USE_FBO_FOR_PICKING){
        if(!fboForPicking){
            glGenFramebuffers(1, &fboForPicking);
            needToChangeBufferSizeForPicking = true;
        }
        glBindFramebuffer(GL_FRAMEBUFFER, fboForPicking);

        if(needToChangeBufferSizeForPicking){
            // color buffer
            if(colorBufferForPicking){
                glDeleteRenderbuffers(1, &colorBufferForPicking);
            }
            glGenRenderbuffers(1, &colorBufferForPicking);
            glBindRenderbuffer(GL_RENDERBUFFER, colorBufferForPicking);
            glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA, viewportWidth, viewportHeight);
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, colorBufferForPicking);
            
            // depth buffer
            if(depthBufferForPicking){
                glDeleteRenderbuffers(1, &depthBufferForPicking);
            }
            glGenRenderbuffers(1, &depthBufferForPicking);
            glBindRenderbuffer(GL_RENDERBUFFER, depthBufferForPicking);
            glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT, viewportWidth, viewportHeight);
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depthBufferForPicking);
            
            needToChangeBufferSizeForPicking = false;
        }
    }
    
    self->extractPreprocessedNodes();

    GLboolean isMultiSampleEnabled;
    if(!USE_FBO_FOR_PICKING){
        isMultiSampleEnabled = glIsEnabled(GL_MULTISAMPLE);
        if(isMultiSampleEnabled){
            glDisable(GL_MULTISAMPLE);
        }
    }
    
    if(!SHOW_IMAGE_FOR_PICKING){
        glScissor(x, y, 1, 1);
        glEnable(GL_SCISSOR_TEST);
    }

    isPicking = true;
    isActuallyRendering = false;
    beginRendering();
    pushProgram(solidColorProgram, false);
    currentNodePath.clear();
    pickingNodePathList.clear();

    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    renderScene();
    
    popProgram();
    isPicking = false;

    glDisable(GL_SCISSOR_TEST);

    endRendering();

    if(!USE_FBO_FOR_PICKING){
        if(isMultiSampleEnabled){
            glEnable(GL_MULTISAMPLE);
        }
        glBindFramebuffer(GL_READ_FRAMEBUFFER, fboForPicking);
        glReadBuffer(GL_COLOR_ATTACHMENT0);
    }
    
    GLfloat color[4];
    glReadPixels(x, y, 1, 1, GL_RGBA, GL_FLOAT, color);
    if(SHOW_IMAGE_FOR_PICKING){
        color[2] = 0.0f;
    }
    unsigned int id = (color[0] * 255) + ((int)(color[1] * 255) << 8) + ((int)(color[2] * 255) << 16) - 1;

    pickedNodePath.clear();

    if(0 < id && id < pickingNodePathList.size()){
        GLfloat depth;
        glReadPixels(x, y, 1, 1, GL_DEPTH_COMPONENT, GL_FLOAT, &depth);
        Vector3 projected;
        if(self->unproject(x, y, depth, pickedPoint)){
            pickedNodePath = *pickingNodePathList[id];
        }
    }

    if(USE_FBO_FOR_PICKING){
        glBindFramebuffer(GL_FRAMEBUFFER, defaultFBO);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, defaultFBO);
    }

    return !pickedNodePath.empty();
}


void GLSLSceneRendererImpl::renderScene()
{
    SgCamera* camera = self->currentCamera();
    if(camera){
        renderCamera(camera, self->currentCameraPosition());

        postRenderingFunctions.clear();
        transparentRenderingFunctions.clear();

        renderSceneGraphNodes();

        for(auto&& func : postRenderingFunctions){
            func();
        }
        postRenderingFunctions.clear();
        
        if(!transparentRenderingFunctions.empty()){
            renderTransparentObjects();
        }
    }
}


bool GLSLSceneRendererImpl::renderShadowMap(int lightIndex)
{
    SgLight* light;
    Affine3 T;
    self->getLightInfo(lightIndex, light, T);
    if(light && light->on()){
        SgCamera* shadowMapCamera = phongShadowLightingProgram.getShadowMapCamera(light, T);
        if(shadowMapCamera){
            renderCamera(shadowMapCamera, T);
            phongShadowLightingProgram.setShadowMapViewProjection(PV);
            renderSceneGraphNodes();
            glFlush();
            glFinish();
            return true;
        }
    }
    return false;
}
    

void GLSLSceneRendererImpl::renderCamera(SgCamera* camera, const Affine3& cameraPosition)
{
    if(SgPerspectiveCamera* pers = dynamic_cast<SgPerspectiveCamera*>(camera)){
        double aspectRatio = self->aspectRatio();
        self->getPerspectiveProjectionMatrix(
            pers->fovy(aspectRatio), aspectRatio, pers->nearClipDistance(), pers->farClipDistance(),
            projectionMatrix);
        
    } else if(SgOrthographicCamera* ortho = dynamic_cast<SgOrthographicCamera*>(camera)){
        GLfloat left, right, bottom, top;
        self->getViewVolume(ortho, left, right, bottom, top);
        self->getOrthographicProjectionMatrix(
            left, right, bottom, top, ortho->nearClipDistance(), ortho->farClipDistance(),
            projectionMatrix);
        
    } else {
        self->getPerspectiveProjectionMatrix(
            radian(40.0), self->aspectRatio(), 0.01, 1.0e4,
            projectionMatrix);
    }

    if(isUpsideDownEnabled){
        Affine3 T = cameraPosition * AngleAxis(PI, Vector3(0.0, 0.0, 1.0));
        viewMatrix = T.inverse(Eigen::Isometry);
    } else {
        viewMatrix = cameraPosition.inverse(Eigen::Isometry);
    }
    PV = projectionMatrix * viewMatrix.matrix();

    modelMatrixStack.clear();
    modelMatrixStack.push_back(Affine3::Identity());
}


void GLSLSceneRendererImpl::beginRendering()
{
    isCheckingUnusedResources = isPicking ? false : doUnusedResourceCheck;

    if(isResourceClearRequested){
        resourceMaps[0].clear();
        resourceMaps[1].clear();
        hasValidNextResourceMap = false;
        isCheckingUnusedResources = false;
        isResourceClearRequested = false; 
    }
    if(hasValidNextResourceMap){
        currentResourceMapIndex = 1 - currentResourceMapIndex;
        currentResourceMap = &resourceMaps[currentResourceMapIndex];
        nextResourceMap = &resourceMaps[1 - currentResourceMapIndex];
        hasValidNextResourceMap = false;
    }
}


void GLSLSceneRendererImpl::endRendering()
{
    if(isCheckingUnusedResources){
        currentResourceMap->clear();
        hasValidNextResourceMap = true;
    }
}


void GLSLSceneRendererImpl::renderSceneGraphNodes()
{
    currentProgram->initializeFrameRendering();
    clearGLState();

    if(currentLightingProgram){
        renderLights(currentLightingProgram);
        renderFog(currentLightingProgram);
    }

    renderingFunctions.dispatch(self->sceneRoot());
}


void GLSLSceneRenderer::renderLights(LightingProgram* program)
{
    impl->renderLights(program);
}


void GLSLSceneRendererImpl::renderLights(LightingProgram* program)
{
    int lightIndex = 0;

    const int n = self->numLights();
    for(int i=0; i < n; ++i){
        if(lightIndex == program->maxNumLights()){
            break;
        }
        SgLight* light;
        Affine3 T;
        self->getLightInfo(i, light, T);
        if(light->on()){
            bool isCastingShadow = (shadowLightIndices.find(i) != shadowLightIndices.end());
            if(program->setLight(lightIndex, light, T, viewMatrix, isCastingShadow)){
                ++lightIndex;
            }
        }
    }

    if(lightIndex < program->maxNumLights()){
        SgLight* headLight = self->headLight();
        if(headLight->on()){
            if(program->setLight(
                   lightIndex, headLight, self->currentCameraPosition(), viewMatrix, false)){
                ++lightIndex;
            }
        }
    }

    program->setNumLights(lightIndex);
}


void GLSLSceneRenderer::renderFog(LightingProgram* program)
{
    impl->renderFog(program);
}


void GLSLSceneRendererImpl::renderFog(LightingProgram* program)
{
    SgFog* fog = nullptr;
    if(self->isFogEnabled()){
        int n = self->numFogs();
        if(n > 0){
            fog = self->fog(n - 1); // use the last fog
        }
    }
    if(fog != prevFog){
        isCurrentFogUpdated = true;
        if(!fog){
            currentFogConnection.disconnect();
        } else {
            currentFogConnection.reset(
                fog->sigUpdated().connect(
                    [&](const SgUpdate&){
                        if(!self->isFogEnabled()){
                            currentFogConnection.disconnect();
                        }
                        isCurrentFogUpdated = true;
                    }));
        }
    }

    if(isCurrentFogUpdated){
        program->setFog(fog);
    }
    isCurrentFogUpdated = false;
    prevFog = fog;
}


const Affine3& GLSLSceneRenderer::currentModelTransform() const
{
    return impl->modelMatrixStack.back();
}


const Matrix4& GLSLSceneRenderer::projectionMatrix() const
{
    return impl->projectionMatrix;
}


const Matrix4& GLSLSceneRenderer::viewProjectionMatrix() const
{
    return impl->PV;
}


Matrix4 GLSLSceneRenderer::modelViewMatrix() const
{
    return impl->viewMatrix * impl->modelMatrixStack.back().matrix();
}


Matrix4 GLSLSceneRenderer::modelViewProjectionMatrix() const
{
    return impl->PV * impl->modelMatrixStack.back().matrix();
}


bool GLSLSceneRenderer::isPicking() const
{
    return impl->isPicking;
}


void GLSLSceneRendererImpl::pushProgram(ShaderProgram& program, bool isLightingProgram)
{
    ProgramInfo info;
    info.program = currentProgram;
    info.lightingProgram = currentLightingProgram;
    info.nolightingProgram = currentNolightingProgram;
    
    if(&program != currentProgram){
        if(currentProgram){
            currentProgram->deactivate();
        }
        currentProgram = &program;
        if(isLightingProgram){
            currentLightingProgram = static_cast<LightingProgram*>(currentProgram);
            currentNolightingProgram = nullptr;
        } else {
            currentLightingProgram = nullptr;
            currentNolightingProgram = static_cast<NolightingProgram*>(currentProgram);
        }
        program.activate();

        clearGLState();
    }
    programStack.push_back(info);
}


void GLSLSceneRenderer::pushShaderProgram(ShaderProgram& program, bool isLightingProgram)
{
    impl->pushProgram(program, isLightingProgram);
}


void GLSLSceneRendererImpl::popProgram()
{
    ProgramInfo& info = programStack.back();
    if(info.program != currentProgram){
        if(currentProgram){
            currentProgram->deactivate();
        }
        currentProgram = info.program;
        currentLightingProgram = info.lightingProgram;
        currentNolightingProgram = info.nolightingProgram;
        if(currentProgram){
            currentProgram->activate();
            clearGLState();
        }
    }
    programStack.pop_back();
}


void GLSLSceneRenderer::popShaderProgram()
{
    impl->popProgram();
}


const std::vector<SgNode*>& GLSLSceneRenderer::pickedNodePath() const
{
    return impl->pickedNodePath;
}


const Vector3& GLSLSceneRenderer::pickedPoint() const
{
    return impl->pickedPoint;
}


inline void GLSLSceneRendererImpl::setPickColor(unsigned int id)
{
    Vector3f color;
    color[0] = (id & 0xff) / 255.0;
    color[1] = ((id >> 8) & 0xff) / 255.0;
    color[2] = ((id >> 16) & 0xff) / 255.0;
    if(SHOW_IMAGE_FOR_PICKING){
        color[2] = 1.0f;
    }
    solidColorProgram.setColor(color);
}
        

/**
   @return id of the current object
*/
inline unsigned int GLSLSceneRendererImpl::pushPickId(SgNode* node, bool doSetColor)
{
    unsigned int id = 0;
    
    if(isPicking){
        id = pickingNodePathList.size() + 1;
        currentNodePath.push_back(node);
        pickingNodePathList.push_back(std::make_shared<SgNodePath>(currentNodePath));
        if(doSetColor){
            setPickColor(id);
        }
    }

    return id;
}


inline void GLSLSceneRendererImpl::popPickId()
{
    if(isPicking){
        currentNodePath.pop_back();
    }
}


void GLSLSceneRenderer::renderNode(SgNode* node)
{
    impl->renderingFunctions.dispatch(node);
}


void GLSLSceneRendererImpl::renderGroup(SgGroup* group)
{
    pushPickId(group);
    renderChildNodes(group);
    popPickId();
}


void GLSLSceneRenderer::renderCustomGroup(SgGroup* group, std::function<void()> traverseFunction)
{
    impl->pushPickId(group);
    traverseFunction();
    impl->popPickId();
}


void GLSLSceneRendererImpl::renderSwitch(SgSwitch* node)
{
    if(node->isTurnedOn()){
        renderGroup(node);
    }
}


void GLSLSceneRendererImpl::renderUnpickableGroup(SgUnpickableGroup* group)
{
    if(!isPicking){
        renderGroup(group);
    }
}


void GLSLSceneRendererImpl::renderTransform(SgTransform* transform)
{
    Affine3 T;
    transform->getTransform(T);
    modelMatrixStack.push_back(modelMatrixStack.back() * T);
    pushPickId(transform);

    renderChildNodes(transform);

    popPickId();
    modelMatrixStack.pop_back();
}


void GLSLSceneRenderer::renderCustomTransform(SgTransform* transform, std::function<void()> traverseFunction)
{
    Affine3 T;
    transform->getTransform(T);
    impl->modelMatrixStack.push_back(impl->modelMatrixStack.back() * T);
    impl->pushPickId(transform);

    traverseFunction();

    impl->popPickId();
    impl->modelMatrixStack.pop_back();
}    
    

VertexResource* GLSLSceneRendererImpl::getOrCreateVertexResource(SgObject* obj)
{
    VertexResource* resource;
    auto p = currentResourceMap->find(obj);
    if(p == currentResourceMap->end()){
        resource = new VertexResource(this, obj);
        p = currentResourceMap->insert(GLResourceMap::value_type(obj, resource)).first;
    } else {
        resource = static_cast<VertexResource*>(p->second.get());
    }

    if(isCheckingUnusedResources){
        nextResourceMap->insert(*p);
    }

    return resource;
}


void GLSLSceneRendererImpl::drawVertexResource(VertexResource* resource, GLenum primitiveMode, const Affine3& position)
{
    currentProgram->setTransform(PV, viewMatrix, position, resource->pLocalTransform);
    glBindVertexArray(resource->vao);
    glDrawArrays(primitiveMode, 0, resource->numVertices);
}


void GLSLSceneRendererImpl::renderShape(SgShape* shape)
{
    SgMesh* mesh = shape->mesh();
    if(mesh && mesh->hasVertices()){
        SgMaterial* material = shape->material();
        if(material && material->transparency() > 0.0){
            if(!isRenderingShadowMap){
                const Affine3& position = modelMatrixStack.back();
                auto pickId = pushPickId(shape, false);
                transparentRenderingFunctions.push_back(
                    [this, shape, position, pickId](){
                        renderShapeMain(shape, position, pickId); });
                popPickId();
            }
        } else {
            auto pickId = pushPickId(shape, false);
            renderShapeMain(shape, modelMatrixStack.back(), pickId);
            popPickId();
        }
    }
}


void GLSLSceneRendererImpl::renderShapeMain(SgShape* shape, const Affine3& position, unsigned int pickId)
{
    SgMesh* mesh = shape->mesh();
    bool isTextureValid = false;
    
    if(isPicking){
        setPickColor(pickId);
    } else {
        renderMaterial(shape->material());
        if(currentLightingProgram == &phongShadowLightingProgram){
            if(shape->texture() && mesh->hasTexCoords()){
                isTextureValid = renderTexture(shape->texture());
            }
            phongShadowLightingProgram.setTextureEnabled(isTextureValid);
            phongShadowLightingProgram.setVertexColorEnabled(mesh->hasColors());
        }
    }

    VertexResource* resource = getOrCreateVertexResource(mesh);
    if(!resource->isValid()){
        writeMeshVertices(mesh, resource, isTextureValid ? shape->texture() : nullptr);
    }

    if(!isRenderingShadowMap){

        if(!stateFlag[CULL_FACE]){
            bool enableCullFace;
            switch(backFaceCullingMode){
            case GLSceneRenderer::ENABLE_BACK_FACE_CULLING:
                enableCullFace = mesh->isSolid();
                break;
            case GLSceneRenderer::DISABLE_BACK_FACE_CULLING:
                enableCullFace = false;
                break;
            case GLSceneRenderer::FORCE_BACK_FACE_CULLING:
            default:
                enableCullFace = true;
                break;
            }
            if(enableCullFace){
                glEnable(GL_CULL_FACE);
            } else {
                glDisable(GL_CULL_FACE);
            }
            isCullFaceEnabled = enableCullFace;
            stateFlag[CULL_FACE] = true;

        } else if(backFaceCullingMode == GLSceneRenderer::ENABLE_BACK_FACE_CULLING){
            if(mesh->isSolid()){
                if(!isCullFaceEnabled){
                    glEnable(GL_CULL_FACE);
                    isCullFaceEnabled = true;
                }
            } else {
                if(isCullFaceEnabled){
                    glDisable(GL_CULL_FACE);
                    isCullFaceEnabled = false;
                }
            }
        }
    }
    
    drawVertexResource(resource, GL_TRIANGLES, position);

    if(isNormalVisualizationEnabled && isActuallyRendering && resource->normalVisualization){
        renderLineSet(resource->normalVisualization);
    }
}


void GLSLSceneRenderer::dispatchToTransparentPhase(std::function<void()> renderingFunction)
{
    impl->transparentRenderingFunctions.push_back(renderingFunction);
}


void GLSLSceneRendererImpl::renderTransparentObjects()
{
    if(!isPicking){
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glDepthMask(GL_FALSE);
    }

    const int n = transparentRenderingFunctions.size();
    for(int i=0; i < n; ++i){
        transparentRenderingFunctions[i]();
    }

    if(!isPicking){
        glDisable(GL_BLEND);
        glDepthMask(GL_TRUE);
    }

    transparentRenderingFunctions.clear();
}


void GLSLSceneRendererImpl::renderMaterial(const SgMaterial* material)
{
    currentProgram->setMaterial(material ? material : defaultMaterial);
}


bool GLSLSceneRendererImpl::renderTexture(SgTexture* texture)
{
    SgImage* sgImage = texture->image();
    if(!sgImage || sgImage->empty()){
        return false;
    }

    auto p = currentResourceMap->find(sgImage);
    TextureResource* resource;
    if(p != currentResourceMap->end()){
        resource = static_cast<TextureResource*>(p->second.get());
        if(resource->isLoaded){
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, resource->textureId);
            glBindSampler(0, resource->samplerId);
            if(resource->isImageUpdateNeeded){
                loadTextureImage(resource, sgImage->constImage());
            }
        }
    } else {
        resource = new TextureResource;
        currentResourceMap->insert(GLResourceMap::value_type(sgImage, resource));

        GLuint samplerId;
        glActiveTexture(GL_TEXTURE0);
        glGenTextures(1, &resource->textureId);
        glBindTexture(GL_TEXTURE_2D, resource->textureId);

        if(loadTextureImage(resource, sgImage->constImage())){
            glGenSamplers(1, &samplerId);
            glBindSampler(0, samplerId);
            glSamplerParameteri(samplerId, GL_TEXTURE_WRAP_S, texture->repeatS() ? GL_REPEAT : GL_CLAMP_TO_EDGE);
            glSamplerParameteri(samplerId, GL_TEXTURE_WRAP_T, texture->repeatT() ? GL_REPEAT : GL_CLAMP_TO_EDGE);
            glSamplerParameteri(samplerId, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glSamplerParameteri(samplerId, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
            resource->samplerId = samplerId;
        }
    }

    if(isCheckingUnusedResources){
        nextResourceMap->insert(GLResourceMap::value_type(sgImage, resource)); 
    }

    return resource->isLoaded;
}


bool GLSLSceneRendererImpl::loadTextureImage(TextureResource* resource, const Image& image)
{
    GLenum format = GL_RGB;
    switch(image.numComponents()){
    case 1 : format = GL_RED; break;
    case 2 : format = GL_RG; break;
    case 3 : format = GL_RGB; break;
    case 4 : format = GL_RGBA; break;
    default:
        resource->clear();
        return false;
    }
    
    if(image.numComponents() == 3){
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    } else {
        glPixelStorei(GL_UNPACK_ALIGNMENT, image.numComponents());
    }
    resource->numComponents = image.numComponents();

    const int width = image.width();
    const int height = image.height();

    if(resource->isLoaded && resource->isSameSizeAs(image)){
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, format, GL_UNSIGNED_BYTE, image.pixels());

    } else {
        double w2 = log2(width);
        double h2 = log2(height);
        double pw = ceil(w2);
        double ph = ceil(h2);
        if((pw - w2 == 0.0) && (ph - h2 == 0.0)){
            glTexImage2D(GL_TEXTURE_2D, 0, format, width, height, 0, format, GL_UNSIGNED_BYTE, image.pixels());
        } else{
            GLsizei potWidth = pow(2.0, pw);
            GLsizei potHeight = pow(2.0, ph);
            scaledImageBuf.resize(potWidth * potHeight * image.numComponents());
            gluScaleImage(format, width, height, GL_UNSIGNED_BYTE, image.pixels(),
                          potWidth, potHeight, GL_UNSIGNED_BYTE, &scaledImageBuf.front());
            glTexImage2D(GL_TEXTURE_2D, 0, format, potWidth, potHeight, 0, format, GL_UNSIGNED_BYTE, &scaledImageBuf.front());
        }
        resource->isLoaded = true;
        resource->width = width;
        resource->height = height;
    }
    glGenerateMipmap(GL_TEXTURE_2D);

    resource->isImageUpdateNeeded = false;

    return true;
}


void GLSLSceneRenderer::onImageUpdated(SgImage* image)
{
    GLResourceMap* resourceMap = impl->hasValidNextResourceMap ? impl->nextResourceMap : impl->currentResourceMap;
    auto p = resourceMap->find(image);
    if(p != resourceMap->end()){
        TextureResource* resource = static_cast<TextureResource*>(p->second.get());
        resource->isImageUpdateNeeded = true;
    }
}


void GLSLSceneRendererImpl::writeMeshVertices(SgMesh* mesh, VertexResource* resource, SgTexture* texture)
{
    const auto& orgVertices = *mesh->vertices();
    auto& triangleVertices = mesh->triangleVertices();
    const int totalNumVertices = triangleVertices.size();
    const int numTriangles = mesh->numTriangles();
    resource->numVertices = totalNumVertices;

    int faceVertexIndex = 0;

    if(USE_GL_SHORT_FOR_VERTICES){
        
        typedef Eigen::Matrix<GLshort,3,1> Vector3s;
        
        /**
           GLShort type is used for storing vertex positions.
           Each value's range is [ -32768, 32767 ], which corresponds to normalized range [ -1, 1 ]
           that covers all the vertex positions.
           In the vertex shader, [ -1, 1 ] value is converted to the original position
        */
        vector<Vector3s> normalizedVertices;
        
        normalizedVertices.reserve(totalNumVertices);
        BoundingBox bbox = mesh->boundingBox();
        const Vector3 c = bbox.center();
        const Vector3 hs =  0.5 * bbox.size();

        resource->localTransform <<
            hs.x(), 0.0,    0.0,    c.x(),
            0.0,    hs.y(), 0.0,    c.y(),
            0.0,    0.0,    hs.z(), c.z(),
            0.0,    0.0,    0.0,    1.0;
        resource->pLocalTransform = &resource->localTransform;

        const Vector3f cf = c.cast<float>();
        const Vector3f r(32767.0 / hs.x(), 32767.0 / hs.y(), 32767.0 / hs.z());
        
        for(int i=0; i < numTriangles; ++i){
            for(int j=0; j < 3; ++j){
                const int orgVertexIndex = triangleVertices[faceVertexIndex++];
                auto v = orgVertices[orgVertexIndex];
                normalizedVertices.push_back(
                    Vector3s(
                        r.x() * (v.x() - cf.x()),
                        r.y() * (v.y() - cf.y()),
                        r.z() * (v.z() - cf.z())));
            }
        }
        {
            LockVertexArrayAPI lock;
            glBindVertexArray(resource->vao);
            glBindBuffer(GL_ARRAY_BUFFER, resource->newBuffer());
            glVertexAttribPointer((GLuint)0, 3, GL_SHORT, GL_TRUE, 0, ((GLubyte*)NULL + (0)));
        }
        auto size = normalizedVertices.size() * sizeof(Vector3s);
        glBufferData(GL_ARRAY_BUFFER, size, normalizedVertices.data(), GL_STATIC_DRAW);

    } else {
        SgVertexArray vertices;
        vertices.reserve(totalNumVertices);
        for(int i=0; i < numTriangles; ++i){
            for(int j=0; j < 3; ++j){
                const int orgVertexIndex = triangleVertices[faceVertexIndex++];
                vertices.push_back(orgVertices[orgVertexIndex]);
            }
        }
        {
            LockVertexArrayAPI lock;
            glBindVertexArray(resource->vao);
            glBindBuffer(GL_ARRAY_BUFFER, resource->newBuffer());
            glVertexAttribPointer((GLuint)0, 3, GL_FLOAT, GL_FALSE, 0, ((GLubyte*)NULL + (0)));
        }
        glBufferData(GL_ARRAY_BUFFER, vertices.size() * sizeof(Vector3f), vertices.data(), GL_STATIC_DRAW);
    }

    glEnableVertexAttribArray(0);
    
    if(USE_GL_INT_2_10_10_10_REV_FOR_NORMALS){
        writeMeshNormalsPacked(mesh, resource->newBuffer(), resource);
    } else {
        writeMeshNormalsFloat(mesh, resource->newBuffer(), resource);
    }

    if(texture){
        if(USE_GL_HALF_FLOAT_FOR_TEXTURE_COORDINATES){
            writeMeshTexCoordsHalfFloat(mesh, resource->newBuffer(), texture);
        } else if(USE_GL_UNSIGNED_SHORT_FOR_TEXTURE_COORDINATES){
            writeMeshTexCoordsUnsignedShort(mesh, resource->newBuffer(), texture);
        } else {
            writeMeshTexCoordsFloat(mesh, resource->newBuffer(), texture);
        }
    }
    
    if(mesh->hasColors()){
        writeMeshColors(mesh, resource->newBuffer());
    }
}

template<class NormalArrayWrapper>
bool GLSLSceneRendererImpl::writeMeshNormalsSub
(SgMesh* mesh, NormalArrayWrapper& normals, VertexResource* resource)
{
    bool ready = false;
    
    auto& triangleVertices = mesh->triangleVertices();
    const int totalNumVertices = triangleVertices.size();
    const int numTriangles = mesh->numTriangles();
    
    normals.array.reserve(totalNumVertices);

    if(!defaultSmoothShading){
        // flat shading
        const auto& orgVertices = *mesh->vertices();
        for(int i=0; i < numTriangles; ++i){
            SgMesh::TriangleRef triangle = mesh->triangle(i);
            const Vector3f e1 = orgVertices[triangle[1]] - orgVertices[triangle[0]];
            const Vector3f e2 = orgVertices[triangle[2]] - orgVertices[triangle[0]];
            const Vector3f normal = e1.cross(e2).normalized();
            for(int j=0; j < 3; ++j){
                normals.append(normal);
            }
        }
        ready = true;

    } else if(mesh->normals()){
        const auto& orgNormals = *mesh->normals();
        const auto& normalIndices = mesh->normalIndices();
        int faceVertexIndex = 0;
        if(normalIndices.empty()){
            for(int i=0; i < numTriangles; ++i){
                for(int j=0; j < 3; ++j){
                    const int orgVertexIndex = triangleVertices[faceVertexIndex++];
                    normals.append(orgNormals[orgVertexIndex]);
                }
            }
        } else {
            for(int i=0; i < numTriangles; ++i){
                for(int j=0; j < 3; ++j){
                    const int normalIndex = normalIndices[faceVertexIndex++];
                    normals.append(orgNormals[normalIndex]);
                }
            }
        }
        ready = true;
    }
    
    if(isNormalVisualizationEnabled){
        auto lines = new SgLineSet;
        auto lineVertices = lines->getOrCreateVertices();
        const auto& orgVertices = *mesh->vertices();
        int vertexIndex = 0;
        for(int i=0; i < numTriangles; ++i){
            for(int j=0; j < 3; ++j){
                const int orgVertexIndex = triangleVertices[vertexIndex];
                auto& v = orgVertices[orgVertexIndex];
                lineVertices->push_back(v);
                lineVertices->push_back(v + normals.get(vertexIndex) * normalVisualizationLength);
                lines->addLine(vertexIndex * 2, vertexIndex * 2 + 1);
                ++vertexIndex;
            }
        }
        lines->setMaterial(normalVisualizationMaterial);
        resource->normalVisualization = lines;
    }

    return ready;
}    
    

void GLSLSceneRendererImpl::writeMeshNormalsPacked(SgMesh* mesh, GLuint buffer, VertexResource* resource)
{
    struct NormalArrayWrapper {
        vector<uint32_t> array;
        void append(const Vector3f& v){
            const uint32_t xs = v.x() < 0.0f;
            const uint32_t ys = v.y() < 0.0f;
            const uint32_t zs = v.z() < 0.0f;
            array.push_back(
                uint32_t(
                    zs << 29 | ((uint32_t)(v.z() * 511 + (zs << 9)) & 511) << 20 |
                    ys << 19 | ((uint32_t)(v.y() * 511 + (ys << 9)) & 511) << 10 |
                    xs << 9  | ((uint32_t)(v.x() * 511 + (xs << 9)) & 511)));
        }
        Vector3f get(int index){
            auto packed = array[index];
            Vector3f v;
            for(int i=0; i < 3; ++i){
                if(packed & 512){ // minus
                    v[i] = (static_cast<int>(packed & 511) - 512) / 512.0f;
                } else { // plus
                    v[i] = (packed & 511) / 511.0f;
                }
                packed >>= 10;
            }
            return v;
        }
    } normals;
            
    if(writeMeshNormalsSub(mesh, normals, resource)){
        {
            LockVertexArrayAPI lock;
            glBindBuffer(GL_ARRAY_BUFFER, buffer);
            glVertexAttribPointer((GLuint)1, 4, GL_INT_2_10_10_10_REV, GL_TRUE, 0, ((GLubyte*)NULL + (0)));
        }
        glBufferData(GL_ARRAY_BUFFER, normals.array.size() * sizeof(uint32_t), normals.array.data(), GL_STATIC_DRAW);
        glEnableVertexAttribArray(1);
    }
}


void GLSLSceneRendererImpl::writeMeshNormalsFloat(SgMesh* mesh, GLuint buffer, VertexResource* resource)
{
    struct NormalArrayWrapper {
        SgNormalArray array;
        void append(const Vector3f& v){ array.push_back(v); }
        Vector3f get(int index){ return array[index]; }
    } normals;
            
    if(writeMeshNormalsSub(mesh, normals, resource)){
        {
            LockVertexArrayAPI lock;
            glBindBuffer(GL_ARRAY_BUFFER, buffer);
            glVertexAttribPointer((GLuint)1, 3, GL_FLOAT, GL_FALSE, 0, ((GLubyte*)NULL + (0)));
        }
        glBufferData(GL_ARRAY_BUFFER, normals.array.size() * sizeof(Vector3f), normals.array.data(), GL_STATIC_DRAW);
        glEnableVertexAttribArray(1);
    }
}


template<typename value_type, GLenum gltype, GLboolean normalized, class TexCoordArrayWrapper>
void GLSLSceneRendererImpl::writeMeshTexCoordsSub
(SgMesh* mesh, GLuint buffer, SgTexture* texture, TexCoordArrayWrapper& texCoords)
{
    auto& triangleVertices = mesh->triangleVertices();
    const int totalNumVertices = triangleVertices.size();
    SgTexCoordArrayPtr pOrgTexCoords;
    const auto& texCoordIndices = mesh->texCoordIndices();

    auto tt = texture->textureTransform();
    if(!tt){
        pOrgTexCoords = mesh->texCoords();
    } else {
        Eigen::Rotation2Df R(tt->rotation());
        const auto& c = tt->center();
        Eigen::Translation<float, 2> C(c.x(), c.y());
        const auto& t = tt->translation();
        Eigen::Translation<float, 2> T(t.x(), t.y());
        const auto s = tt->scale().cast<float>();
        Eigen::Affine2f M = C.inverse() * Eigen::Scaling(s.x(), s.y()) * R * C * T;

        const auto& orgTexCoords = *mesh->texCoords();
        const size_t n = orgTexCoords.size();
        pOrgTexCoords = new SgTexCoordArray(n);
        for(size_t i=0; i < n; ++i){
            (*pOrgTexCoords)[i] = M * orgTexCoords[i];
        }
    }

    texCoords.array.reserve(totalNumVertices);
    const int numTriangles = mesh->numTriangles();
    int faceVertexIndex = 0;
    
    if(texCoordIndices.empty()){
        for(int i=0; i < numTriangles; ++i){
            for(int j=0; j < 3; ++j){
                const int orgVertexIndex = triangleVertices[faceVertexIndex++];
                texCoords.append((*pOrgTexCoords)[orgVertexIndex]);
            }
        }
    } else {
        for(int i=0; i < numTriangles; ++i){
            for(int j=0; j < 3; ++j){
                const int texCoordIndex = texCoordIndices[faceVertexIndex++];
                texCoords.append((*pOrgTexCoords)[texCoordIndex]);
            }
        }
    }
    {
        LockVertexArrayAPI lock;
        glBindBuffer(GL_ARRAY_BUFFER, buffer);
        glVertexAttribPointer((GLuint)2, 2, gltype, normalized, 0, 0);
    }
    auto size = texCoords.array.size() * sizeof(value_type);
    glBufferData(GL_ARRAY_BUFFER, size, texCoords.array.data(), GL_STATIC_DRAW);
    glEnableVertexAttribArray(2);
}


void GLSLSceneRendererImpl::writeMeshTexCoordsHalfFloat(SgMesh* mesh, GLuint buffer, SgTexture* texture)
{
    typedef Eigen::Matrix<GLhalf,2,1> Vector2h;

    struct TexCoordArrayWrapper {
        vector<Vector2h> array;
        /**
           Qt provides 16-bit floating point support since version 5.11,
           and the qFloatToFloat16 function can be used by including the <QFloat16> header.
           It may be better to use it the corresponding Qt version is available.
        */
        GLhalf toHalf(const float& value){
            uint32_t x = *((uint32_t*)&value);
            uint32_t e = x & 0x7f800000;
            if(e == 0 || e < 0x38800000){
                return 0;
            } else if(e >0x47000000){
                return 0x7bff;
            }
            return ((x >> 16) & 0x8000) | ((x & 0x7fffffff) >> 13) - 0x1c000;
        }
        void append(const Vector2f& uv){
            array.push_back(Vector2h(toHalf(uv[0]), toHalf(uv[1])));
        }
    } texCoords;

    writeMeshTexCoordsSub<Vector2h, GL_HALF_FLOAT, GL_FALSE>(mesh, buffer, texture, texCoords);
}


void GLSLSceneRendererImpl::writeMeshTexCoordsUnsignedShort(SgMesh* mesh, GLuint buffer, SgTexture* texture)
{
    typedef Eigen::Matrix<GLushort,2,1> Vector2us;
    
    struct TexCoordArrayWrapper {
        vector<Vector2us> array;
        float repeat(float v){
            if(v < 0.0f){
                return v - floor(v);
            } else if(v > 1.0f){
                return v - floor(v);
            }
            return v;
        }
        void append(const Vector2f& uv){
            array.push_back(Vector2us(65535.0f * repeat(uv[0]), 65535.0f * repeat(uv[1])));
        }
    } texCoords;

    writeMeshTexCoordsSub<Vector2us, GL_UNSIGNED_SHORT, GL_TRUE>(mesh, buffer, texture, texCoords);
}


void GLSLSceneRendererImpl::writeMeshTexCoordsFloat(SgMesh* mesh, GLuint buffer, SgTexture* texture)
{
    struct TexCoordArrayWrapper {
        SgTexCoordArray array;
        void append(const Vector2f& uv){
            array.push_back(uv);
        }
    } texCoords;

    writeMeshTexCoordsSub<Vector2f, GL_FLOAT, GL_FALSE>(mesh, buffer, texture, texCoords);
}


void GLSLSceneRendererImpl::writeMeshColors(SgMesh* mesh, GLuint buffer)
{
    auto& triangleVertices = mesh->triangleVertices();
    const int totalNumVertices = triangleVertices.size();
    const auto& orgColors = *mesh->colors();
    const auto& colorIndices = mesh->colorIndices();

    typedef Eigen::Array<GLubyte,3,1> Color;
    vector<Color> colors;
    colors.reserve(totalNumVertices);
    
    const int numTriangles = mesh->numTriangles();
    int faceVertexIndex = 0;

    if(colorIndices.empty()){
        for(int i=0; i < numTriangles; ++i){
            for(int j=0; j < 3; ++j){
                const int orgVertexIndex = triangleVertices[faceVertexIndex++];
                Vector3f c = 255.0f * orgColors[orgVertexIndex];
                colors.push_back(Color(c[0], c[1], c[2]));
            }
        }
    } else {
        for(int i=0; i < numTriangles; ++i){
            for(int j=0; j < 3; ++j){
                const int colorIndex = colorIndices[faceVertexIndex++];
                Vector3f c = 255.0f * orgColors[colorIndex];
                colors.push_back(Color(c[0], c[1], c[2]));
            }
        }
    }

    {
        LockVertexArrayAPI lock;
        glBindBuffer(GL_ARRAY_BUFFER, buffer);
        glVertexAttribPointer((GLuint)3, 3, GL_UNSIGNED_BYTE, GL_TRUE, 0, ((GLubyte*)NULL + (0)));
    }
    glBufferData(GL_ARRAY_BUFFER, colors.size() * sizeof(Color), colors.data(), GL_STATIC_DRAW);
    glEnableVertexAttribArray(3);
}
    

void GLSLSceneRendererImpl::renderPointSet(SgPointSet* pointSet)
{
    if(!pointSet->hasVertices()){
        return;
    }

    pushProgram(solidColorProgram, false);

    const double s = pointSet->pointSize();
    if(s > 0.0){
        setPointSize(s);
    } else {
        setPointSize(defaultPointSize);
    }
    
    renderPlot(pointSet, GL_POINTS,
               [pointSet]() -> SgVertexArrayPtr { return pointSet->vertices(); });

    popProgram();
}


void GLSLSceneRendererImpl::renderPlot
(SgPlot* plot, GLenum primitiveMode, std::function<SgVertexArrayPtr()> getVertices)
{
    pushPickId(plot);

    bool hasColors = plot->hasColors();
    
    if(isPicking){
        solidColorProgram.enableColorArray(false);
    } else {
        if(!hasColors){
            renderMaterial(plot->material());
        }
        solidColorProgram.enableColorArray(hasColors);
    }
    
    VertexResource* resource = getOrCreateVertexResource(plot);
    if(!resource->isValid()){
        glBindVertexArray(resource->vao);
        SgVertexArrayPtr vertices = getVertices();
        const size_t n = vertices->size();
        resource->numVertices = n;

        {
            LockVertexArrayAPI lock;
            glBindBuffer(GL_ARRAY_BUFFER, resource->newBuffer());
            glVertexAttribPointer((GLuint)0, 3, GL_FLOAT, GL_FALSE, 0, ((GLubyte *)NULL + (0)));
        }
        glBufferData(GL_ARRAY_BUFFER, vertices->size() * sizeof(Vector3f), vertices->data(), GL_STATIC_DRAW);
        glEnableVertexAttribArray(0);

        if(hasColors){
            typedef Eigen::Array<GLubyte,3,1> Color;
            vector<Color> colors;
            colors.reserve(n);
            const SgColorArray& orgColors = *plot->colors();
            const SgIndexArray& colorIndices = plot->colorIndices();
            size_t i = 0;
            if(plot->colorIndices().empty()){
                const size_t m = std::min(n, orgColors.size());
                while(i < m){
                    Vector3f c = 255.0f * orgColors[i];
                    colors.push_back(Color(c[0], c[1], c[2]));
                    ++i;
                }
            } else {
                const size_t m = std::min(n, colorIndices.size());
                size_t i = 0;
                while(i < m){
                    Vector3f c = 255.0f * orgColors[colorIndices[i]];
                    colors.push_back(Color(c[0], c[1], c[2]));
                    ++i;
                }
            }
            if(i < n){
                const auto& c = colors.back();
                while(i < n){
                    colors.push_back(c);
                    ++i;
                }
            }
            {
                LockVertexArrayAPI lock;
                glBindBuffer(GL_ARRAY_BUFFER, resource->newBuffer());
                glVertexAttribPointer((GLuint)1, 3, GL_UNSIGNED_BYTE, GL_TRUE, 0, ((GLubyte*)NULL +(0)));
            }
            glBufferData(GL_ARRAY_BUFFER, n * sizeof(Color), colors.data(), GL_STATIC_DRAW);
            glEnableVertexAttribArray(1);
        }
    }        

    drawVertexResource(resource, primitiveMode, modelMatrixStack.back());
    
    popPickId();
}


static SgVertexArrayPtr getLineSetVertices(SgLineSet* lineSet)
{
    const SgVertexArray& orgVertices = *lineSet->vertices();
    SgVertexArray* vertices = new SgVertexArray;
    const int n = lineSet->numLines();
    vertices->reserve(n * 2);
    for(int i=0; i < n; ++i){
        SgLineSet::LineRef line = lineSet->line(i);
        vertices->push_back(orgVertices[line[0]]);
        vertices->push_back(orgVertices[line[1]]);
    }
    return vertices;
}


void GLSLSceneRendererImpl::renderLineSet(SgLineSet* lineSet)
{
    if(isRenderingShadowMap){
        return;
    }
    
    if(!lineSet->hasVertices() || lineSet->numLines() <= 0){
        return;
    }

    pushProgram(solidColorProgram, false);
    
    const double w = lineSet->lineWidth();
    if(w > 0.0){
        setLineWidth(w);
    } else {
        setLineWidth(defaultLineWidth);
    }

    renderPlot(lineSet, GL_LINES,
               [lineSet](){ return getLineSetVertices(lineSet); });

    popProgram();
}


void GLSLSceneRendererImpl::renderOverlay(SgOverlay* overlay)
{
    if(!isActuallyRendering){
        return;
    }

    pushProgram(solidColorProgram, false);
    modelMatrixStack.push_back(Affine3::Identity());

    const Matrix4 PV0 = PV;
    SgOverlay::ViewVolume v;
    const Array4i vp = self->viewport();
    overlay->calcViewVolume(vp[2], vp[3], v);
    self->getOrthographicProjectionMatrix(v.left, v.right, v.bottom, v.top, v.zNear, v.zFar, PV);
            
    renderGroup(overlay);

    PV = PV0;
    modelMatrixStack.pop_back();
    popProgram();
}


void GLSLSceneRendererImpl::renderOutlineGroup(SgOutlineGroup* outline)
{
    if(isPicking){
        renderGroup(outline);
    } else {
        const Affine3& T = modelMatrixStack.back();
        postRenderingFunctions.push_back(
            [this, outline, T](){ renderOutlineGroupMain(outline, T); });
    }
}


void GLSLSceneRendererImpl::renderOutlineGroupMain(SgOutlineGroup* outline, const Affine3& T)
{
    modelMatrixStack.push_back(T);

    glClearStencil(0);
    glClear(GL_STENCIL_BUFFER_BIT);
    glEnable(GL_STENCIL_TEST);
    glStencilFunc(GL_ALWAYS, 1, -1);
    glStencilOp(GL_KEEP, GL_REPLACE, GL_REPLACE);

    renderChildNodes(outline);

    glStencilFunc(GL_NOTEQUAL, 1, -1);
    glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);

    float orgLineWidth = lineWidth;
    setLineWidth(outline->lineWidth()*2+1);
    GLint polygonMode;
    glGetIntegerv(GL_POLYGON_MODE, &polygonMode);
    glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);

    pushProgram(solidColorProgram, false);
    solidColorProgram.setColor(outline->color());
    solidColorProgram.setColorChangable(false);
    glDisable(GL_DEPTH_TEST);

    renderChildNodes(outline);

    glEnable(GL_DEPTH_TEST);
    setLineWidth(orgLineWidth);
    glPolygonMode(GL_FRONT_AND_BACK, polygonMode);
    glDisable(GL_STENCIL_TEST);
    solidColorProgram.setColorChangable(true);
    popProgram();

    modelMatrixStack.pop_back();
}


void GLSLSceneRendererImpl::renderSimplifiedRenderingGroup(SgSimplifiedRenderingGroup* group)
{
    if(isRenderingShadowMap){
        return;
    }
    
    if(!isPicking){
        pushProgram(minimumLightingProgram, true);
        if(!isMinimumLightingProgramActivatedInThisFrame){
            renderLights(&minimumLightingProgram);
            isMinimumLightingProgramActivatedInThisFrame = true;
        }
    }

    renderChildNodes(group);

    if(!isPicking){
        popProgram();
    }
}


void GLSLSceneRendererImpl::clearGLState()
{
    std::fill(stateFlag.begin(), stateFlag.end(), false);
    pointSize = defaultPointSize;    
    lineWidth = defaultLineWidth;
}


void GLSLSceneRenderer::setColor(const Vector3f& color)
{
    impl->solidColorProgram.setColor(color);
}


void GLSLSceneRenderer::clearShadows()
{
    impl->shadowLightIndices.clear();
}


void GLSLSceneRenderer::enableShadowOfLight(int index, bool on)
{
    if(on){
        impl->shadowLightIndices.insert(index);
    } else {
        impl->shadowLightIndices.erase(index);
    }
}


void GLSLSceneRenderer::enableShadowAntiAliasing(bool on)
{
    impl->phongShadowLightingProgram.setShadowAntiAliasingEnabled(on);
}


void GLSLSceneRendererImpl::setPointSize(float size)
{
    if(!stateFlag[POINT_SIZE] || pointSize != size){
        float s = isPicking ? std::max(size, MinLineWidthForPicking) : size;
        solidColorProgram.setPointSize(s);
        pointSize = s;
        stateFlag[POINT_SIZE] = true;
    }
}


void GLSLSceneRendererImpl::setLineWidth(float width)
{
    if(!stateFlag[LINE_WIDTH] || lineWidth != width){
        if(isPicking){
            glLineWidth(std::max(width, MinLineWidthForPicking));
        } else {
            glLineWidth(width);
        }
        lineWidth = width;
        stateFlag[LINE_WIDTH] = true;
    }
}


void GLSLSceneRenderer::setLightingMode(int mode)
{
    impl->lightingMode = mode;
}


void GLSLSceneRenderer::setDefaultSmoothShading(bool on)
{
    if(on != impl->defaultSmoothShading){
        impl->defaultSmoothShading = on;
        requestToClearResources();
    }
}


SgMaterial* GLSLSceneRenderer::defaultMaterial()
{
    return impl->defaultMaterial;
}


void GLSLSceneRenderer::enableTexture(bool on)
{
    /*
    if(on != impl->isTextureEnabled){
        impl->isTextureEnabled = on;
    }
    */
}


void GLSLSceneRenderer::setDefaultPointSize(double size)
{
    if(size != impl->defaultPointSize){
        impl->defaultPointSize = size;
    }
}


void GLSLSceneRenderer::setDefaultLineWidth(double width)
{
    if(width != impl->defaultLineWidth){
        impl->defaultLineWidth = width;
    }
}


void GLSLSceneRenderer::showNormalVectors(double length)
{
    bool isEnabled = (length > 0.0);
    if(isEnabled != impl->isNormalVisualizationEnabled || length != impl->normalVisualizationLength){
        impl->isNormalVisualizationEnabled = isEnabled;
        impl->normalVisualizationLength = length;
        requestToClearResources();
    }
}


void GLSLSceneRenderer::enableUnusedResourceCheck(bool on)
{
    if(!on){
        impl->nextResourceMap->clear();
    }
    impl->doUnusedResourceCheck = on;
}


void GLSLSceneRenderer::setUpsideDown(bool on)
{
    impl->isUpsideDownEnabled = on;
}


void GLSLSceneRenderer::setBackFaceCullingMode(int mode)
{
    impl->backFaceCullingMode = mode;
}


int GLSLSceneRenderer::backFaceCullingMode() const
{
    return impl->backFaceCullingMode;
}
