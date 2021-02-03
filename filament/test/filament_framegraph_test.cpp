/*
 * Copyright (C) 2019 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <gtest/gtest.h>

#include "fg/FrameGraph.h"
#include "fg/FrameGraphPassResources.h"
#include "ResourceAllocator.h"

#include <backend/Platform.h>

#include "private/backend/CommandStream.h"

#include "fg2/FrameGraph.h"
#include "fg2/FrameGraphResources.h"
#include "fg2/details/DependencyGraph.h"

using namespace filament;
using namespace backend;

class FrameGraphTest : public testing::Test {
protected:
    CircularBuffer buffer = CircularBuffer{ 8192 };
    Backend gBackend = Backend::NOOP;
    DefaultPlatform* platform = DefaultPlatform::create(&gBackend);
    CommandStream driverApi = CommandStream{ *platform->createDriver(nullptr), buffer };
};

class MockResourceAllocator : public ResourceAllocatorInterface {
    uint32_t handle = 0;
public:
    backend::RenderTargetHandle createRenderTarget(const char* name,
            backend::TargetBufferFlags targetBufferFlags,
            uint32_t width,
            uint32_t height,
            uint8_t samples,
            backend::MRT color,
            backend::TargetBufferInfo depth,
            backend::TargetBufferInfo stencil) noexcept override {
        return backend::RenderTargetHandle(++handle);
    }

    void destroyRenderTarget(backend::RenderTargetHandle h) noexcept override {
    }

    backend::TextureHandle createTexture(const char* name, backend::SamplerType target,
            uint8_t levels,
            backend::TextureFormat format, uint8_t samples, uint32_t width, uint32_t height,
            uint32_t depth, backend::TextureUsage usage) noexcept override {
        return backend::TextureHandle(++handle);
    }

    void destroyTexture(backend::TextureHandle h) noexcept override {
    }
};

struct GenericResource {
    struct Descriptor {};
    void create(ResourceAllocatorInterface&, const char* name, Descriptor const& desc) noexcept {
        id = ++state;
    }
    void destroy(ResourceAllocatorInterface&) noexcept {}
    int id = 0;
private:
    static int state;
};

int GenericResource::state = 0;


TEST_F(FrameGraphTest, SimpleRenderPass) {

    ResourceAllocator resourceAllocator(driverApi);
    FrameGraph fg(resourceAllocator);

    bool renderPassExecuted = false;

    struct RenderPassData {
        FrameGraphId<FrameGraphTexture> output;
        FrameGraphRenderTargetHandle rt;
    };

    auto& renderPass = fg.addPass<RenderPassData>("Render",
            [&](FrameGraph::Builder& builder, auto& data) {
                FrameGraphTexture::Descriptor desc{ .format = TextureFormat::RGBA16F };
                data.output = builder.createTexture("color buffer", desc);
                data.output = builder.write(data.output);

                data.rt = builder.createRenderTarget("color buffer", {
                    .attachments = { data.output } });

                // rendertargets are weird...
                // A) reading from an attachment is equivalent to reading from the RT
                //    so any reads on any attachment should trigger a read on the RT,
                //    meaning the RT can't be culled.
                //    Or maybe, there should be a .use(RT) to declare it's going to be used
                //    in that pass.
                // B) writing to a RT, should propagate to its attachments, but then, how
                //    is the new attachment value returned??
                //    maybe it's a special type of write that doesn't change the version??
                //    or maybe we require writes to be set on the resource.


                EXPECT_TRUE(fg.isValid(data.output));
                EXPECT_TRUE(fg.isValid(data.rt));
            },
            [=, &renderPassExecuted](FrameGraphPassResources const& resources,
                    auto const& data, DriverApi& driver) {
                renderPassExecuted = true;
                auto const& rt = resources.get(data.rt);
                EXPECT_TRUE(rt.target);
                EXPECT_EQ(TargetBufferFlags::COLOR, rt.params.flags.discardStart);
                EXPECT_EQ(TargetBufferFlags::NONE, rt.params.flags.discardEnd);
            });

    fg.present(renderPass.getData().output);    // TODO: this should trigger a read() on the RT
    fg.compile();
    fg.execute(driverApi);

    EXPECT_TRUE(renderPassExecuted);

    resourceAllocator.terminate();
}

TEST_F(FrameGraphTest, SimpleRenderPass2) {

    ResourceAllocator resourceAllocator(driverApi);
    FrameGraph fg(resourceAllocator);

    bool renderPassExecuted = false;

    struct RenderPassData {
        FrameGraphId<FrameGraphTexture> outColor;
        FrameGraphId<FrameGraphTexture> outDepth;
        FrameGraphRenderTargetHandle rt;
    };

    auto& renderPass = fg.addPass<RenderPassData>("Render",
            [&](FrameGraph::Builder& builder, auto& data) {
                data.outColor = builder.createTexture("color buffer",
                        { .format = TextureFormat::RGBA16F });
                data.outDepth = builder.createTexture("depth buffer",
                        { .format = TextureFormat::DEPTH24 });
                data.outColor = builder.write(builder.read(data.outColor));
                data.outDepth = builder.write(builder.read(data.outDepth));
                data.rt = builder.createRenderTarget("rt", {
                        .attachments = { data.outColor, data.outDepth }
                });

                EXPECT_TRUE(fg.isValid(data.outColor));
                EXPECT_TRUE(fg.isValid(data.outDepth));
                EXPECT_TRUE(fg.isValid(data.rt));
            },
            [=, &renderPassExecuted](FrameGraphPassResources const& resources,
                    auto const& data, DriverApi& driver) {
                renderPassExecuted = true;
                auto const& rt = resources.get(data.rt);
                EXPECT_TRUE(rt.target);
                EXPECT_EQ(TargetBufferFlags::COLOR0 | TargetBufferFlags::DEPTH, rt.params.flags.discardStart);
                EXPECT_EQ(TargetBufferFlags::NONE, rt.params.flags.discardEnd);
            });

    fg.present(renderPass.getData().outColor);
    fg.present(renderPass.getData().outDepth);
    fg.compile();
    fg.execute(driverApi);

    EXPECT_TRUE(renderPassExecuted);

    resourceAllocator.terminate();
}

TEST_F(FrameGraphTest, ScenarioDepthPrePass) {

    ResourceAllocator resourceAllocator(driverApi);
    FrameGraph fg(resourceAllocator);

    bool depthPrepassExecuted = false;
    bool colorPassExecuted = false;

    struct DepthPrepassData {
        FrameGraphId<FrameGraphTexture> outDepth;
        FrameGraphRenderTargetHandle rt;
    };

    auto& depthPrepass = fg.addPass<DepthPrepassData>("depth prepass",
            [&](FrameGraph::Builder& builder, auto& data) {
                data.outDepth = builder.createTexture("depth buffer",
                        { .format = TextureFormat::DEPTH24 });
                data.outDepth = builder.write(builder.read(data.outDepth));
                data.rt = builder.createRenderTarget("rt depth", {
                        .attachments = {{}, data.outDepth }
                });
                EXPECT_TRUE(fg.isValid(data.outDepth));
                EXPECT_TRUE(fg.isValid(data.rt));
            },
            [=, &depthPrepassExecuted](FrameGraphPassResources const& resources,
                    auto const& data, DriverApi& driver) {
                depthPrepassExecuted = true;
                auto const& rt = resources.get(data.rt);
                EXPECT_TRUE(rt.target);
                EXPECT_EQ(TargetBufferFlags::DEPTH, rt.params.flags.discardStart);
                EXPECT_EQ(TargetBufferFlags::NONE, rt.params.flags.discardEnd);
            });

    struct ColorPassData {
        FrameGraphId<FrameGraphTexture> outColor;
        FrameGraphId<FrameGraphTexture> outDepth;
        FrameGraphRenderTargetHandle rt;
    };

    auto& colorPass = fg.addPass<ColorPassData>("color pass",
            [&](FrameGraph::Builder& builder, auto& data) {
                data.outColor = builder.createTexture("color buffer",
                        { .format = TextureFormat::RGBA16F });

                // declare a read here, so a reference is added to the previous pass
                data.outDepth = depthPrepass.getData().outDepth;
                data.outColor = builder.write(builder.read(data.outColor));
                data.outDepth = builder.write(builder.read(data.outDepth));
                data.rt = builder.createRenderTarget("rt color+depth", {
                        .attachments = { data.outColor, data.outDepth }
                });

                EXPECT_FALSE(fg.isValid(depthPrepass.getData().outDepth));
                EXPECT_TRUE(fg.isValid(data.outColor));
                EXPECT_TRUE(fg.isValid(data.outDepth));
                EXPECT_TRUE(fg.isValid(data.rt));
            },
            [=, &colorPassExecuted](FrameGraphPassResources const& resources,
                    auto const& data, DriverApi& driver) {
                colorPassExecuted = true;
                auto const& rt = resources.get(data.rt);
                EXPECT_TRUE(rt.target);
                EXPECT_EQ(TargetBufferFlags::COLOR, rt.params.flags.discardStart);
                EXPECT_EQ(TargetBufferFlags::DEPTH, rt.params.flags.discardEnd);
            });

    fg.present(colorPass.getData().outColor);
    fg.compile();
    fg.execute(driverApi);

    EXPECT_TRUE(depthPrepassExecuted);
    EXPECT_TRUE(colorPassExecuted);

    resourceAllocator.terminate();
}

TEST_F(FrameGraphTest, SimplePassCulling) {

    ResourceAllocator resourceAllocator(driverApi);
    FrameGraph fg(resourceAllocator);

    bool renderPassExecuted = false;
    bool postProcessPassExecuted = false;
    bool culledPassExecuted = false;

    struct RenderPassData {
        FrameGraphId<FrameGraphTexture> output;
        FrameGraphRenderTargetHandle rt;
    };

    auto& renderPass = fg.addPass<RenderPassData>("Render",
            [&](FrameGraph::Builder& builder, auto& data) {
                data.output = builder.createTexture("renderTarget");
                data.output = builder.write(data.output);
                data.rt = builder.createRenderTarget("renderTarget", {
                        .attachments = { data.output }
                });
                EXPECT_TRUE(fg.isValid(data.output));
                EXPECT_TRUE(fg.isValid(data.rt));
            },
            [=, &renderPassExecuted](FrameGraphPassResources const& resources,
                    auto const& data, backend::DriverApi& driver) {
                renderPassExecuted = true;
                auto const& rt = resources.get(data.rt);
                EXPECT_TRUE(rt.target);
                EXPECT_EQ(TargetBufferFlags::COLOR, rt.params.flags.discardStart);
                EXPECT_EQ(TargetBufferFlags::NONE, rt.params.flags.discardEnd);
            });


    struct PostProcessPassData {
        FrameGraphId<FrameGraphTexture> input;
        FrameGraphId<FrameGraphTexture> output;
        FrameGraphRenderTargetHandle rt;
    };

    auto& postProcessPass = fg.addPass<PostProcessPassData>("PostProcess",
            [&](FrameGraph::Builder& builder, auto& data) {
                data.input = builder.sample(renderPass.getData().output);
                data.output = builder.createTexture("postprocess-renderTarget");
                data.output = builder.write(data.output);
                data.rt = builder.createRenderTarget("postprocess-renderTarget",{
                    .attachments = { data.output }
                });
                EXPECT_TRUE(fg.isValid(data.input));
                EXPECT_TRUE(fg.isValid(data.output));
                EXPECT_TRUE(fg.isValid(data.rt));
            },
            [=, &postProcessPassExecuted](FrameGraphPassResources const& resources,
                    auto const& data, backend::DriverApi& driver) {
                postProcessPassExecuted = true;
                auto const& rt = resources.get(data.rt);
                EXPECT_TRUE(rt.target);
                EXPECT_EQ(TargetBufferFlags::COLOR, rt.params.flags.discardStart);
                EXPECT_EQ(TargetBufferFlags::NONE, rt.params.flags.discardEnd);
            });


    struct CulledPassData {
        FrameGraphId<FrameGraphTexture> input;
        FrameGraphId<FrameGraphTexture> output;
        FrameGraphRenderTargetHandle rt;
    };

    auto& culledPass = fg.addPass<CulledPassData>("CulledPass",
            [&](FrameGraph::Builder& builder, auto& data) {
                data.input = builder.sample(renderPass.getData().output);
                data.output = builder.createTexture("unused-rendertarget");
                data.output = builder.write(data.output);
                data.rt = builder.createRenderTarget("unused-rendertarget", {
                        .attachments = { data.output }
                });
                EXPECT_TRUE(fg.isValid(data.input));
                EXPECT_TRUE(fg.isValid(data.output));
                EXPECT_TRUE(fg.isValid(data.rt));
            },
            [=, &culledPassExecuted](FrameGraphPassResources const& resources,
                    auto const& data, backend::DriverApi& driver) {
                culledPassExecuted = true;
            });

    fg.present(postProcessPass.getData().output);

    EXPECT_TRUE(fg.isValid(renderPass.getData().output));
    EXPECT_TRUE(fg.isValid(postProcessPass.getData().input));
    EXPECT_TRUE(fg.isValid(postProcessPass.getData().output));
    EXPECT_TRUE(fg.isValid(culledPass.getData().input));
    EXPECT_TRUE(fg.isValid(culledPass.getData().output));

    fg.compile();
    //fg.export_graphviz(utils::slog.d);
    fg.execute(driverApi);

    EXPECT_TRUE(renderPassExecuted);
    EXPECT_TRUE(postProcessPassExecuted);
    EXPECT_FALSE(culledPassExecuted);

    resourceAllocator.terminate();
}

TEST_F(FrameGraphTest, MoveGenericResource) {
    // This checks that:
    // - two passes writing in the same resource, that is replaced (moved) by
    //   another resource, end-up both using the 'replacing' resource.
    // - these passes are not culled, even though they're not consumed by
    //   anyone initially -- but the 'replacing' resource was.
    // - passes writing to the moved resource no longer do (and are culled)

    MockResourceAllocator resourceAllocator;
    FrameGraph fg(resourceAllocator);

    struct RenderPassData {
        FrameGraphId<GenericResource> output;
    };

    bool p0Executed = false;
    bool p1Executed = false;
    bool p2Executed = false;
    bool p3Executed = false;
    int h[4];

    auto& p0 = fg.addPass<RenderPassData>("P0",
            [&](FrameGraph::Builder& builder, auto& data) {
                data.output = builder.create<GenericResource>("r0");
                data.output = builder.write(data.output);
                EXPECT_TRUE(fg.isValid(data.output));
            },
            [&](FrameGraphPassResources const& resources,
                    auto const& data, backend::DriverApi& driver) {
                p0Executed = true;
                h[0] = resources.get(data.output).id;
                EXPECT_TRUE(h[0]);
            });


    auto& p1 = fg.addPass<RenderPassData>("P1",
            [&](FrameGraph::Builder& builder, auto& data) {
                data.output = p0.getData().output;
                data.output = builder.read(data.output);
                data.output = builder.write(data.output);
                EXPECT_TRUE(fg.isValid(data.output));
            },
            [&](FrameGraphPassResources const& resources,
                    auto const& data, backend::DriverApi& driver) {
                p1Executed = true;
                h[1] = resources.get(data.output).id;
                EXPECT_TRUE(h[1]);
            });

    auto& p2 = fg.addPass<RenderPassData>("P2",
            [&](FrameGraph::Builder& builder, auto& data) {
                data.output = builder.create<GenericResource>("r2");
                data.output = builder.write(data.output);
                EXPECT_TRUE(fg.isValid(data.output));
            },
            [&](FrameGraphPassResources const& resources,
                    auto const& data, backend::DriverApi& driver) {
                p2Executed = true;
                h[2] = resources.get(data.output).id;
                EXPECT_TRUE(h[2]);
            });

    auto& p3 = fg.addPass<RenderPassData>("P3",
            [&](FrameGraph::Builder& builder, auto& data) {
                data.output = builder.read(p2.getData().output);
                builder.sideEffect();
                EXPECT_TRUE(fg.isValid(data.output));
            },
            [&](FrameGraphPassResources const& resources,
                    auto const& data, backend::DriverApi& driver) {
                p3Executed = true;
                h[3] = resources.get(data.output).id;
                EXPECT_TRUE(h[3]);
            });

    fg.moveResource(p3.getData().output, p1.getData().output);

    fg.compile();
    fg.execute(driverApi);

    EXPECT_TRUE(p0Executed);
    EXPECT_TRUE(p1Executed);
    EXPECT_FALSE(p2Executed);
    EXPECT_TRUE(p3Executed);
    EXPECT_EQ(h[0], h[1]);
    EXPECT_EQ(h[1], h[3]);
    EXPECT_EQ(h[3], h[0]);
}


class Node : public fg2::DependencyGraph::Node {
    const char *mName;
    bool mCulledCalled = false;
    char const* getName() const override { return mName; }
    void onCulled(fg2::DependencyGraph* graph) override { mCulledCalled = true; }
public:
    Node(fg2::DependencyGraph& graph, const char* name) noexcept : fg2::DependencyGraph::Node(graph), mName(name) { }
    bool isCulledCalled() const noexcept { return mCulledCalled; }
};

TEST(FrameGraph2Test, GraphSimple) {
    using namespace fg2;

    DependencyGraph graph;
    Node* n0 = new Node(graph, "node 0");
    Node* n1 = new Node(graph, "node 1");
    Node* n2 = new Node(graph, "node 2");

    new DependencyGraph::Edge(graph, n0, n1);
    new DependencyGraph::Edge(graph, n1, n2);
    n2->makeTarget();

    graph.cull();

    graph.export_graphviz(utils::slog.d);

    EXPECT_FALSE(n2->isCulled());
    EXPECT_FALSE(n1->isCulled());
    EXPECT_FALSE(n0->isCulled());
    EXPECT_FALSE(n2->isCulledCalled());
    EXPECT_FALSE(n1->isCulledCalled());
    EXPECT_FALSE(n0->isCulledCalled());

    EXPECT_EQ(n0->getRefCount(), 1);
    EXPECT_EQ(n1->getRefCount(), 1);
    EXPECT_EQ(n2->getRefCount(), 1);

    auto edges = graph.getEdges();
    auto nodes = graph.getNodes();
    graph.clear();
    for (auto e : edges) { delete e; }
    for (auto n : nodes) { delete n; }
}

TEST(FrameGraph2Test, GraphCulling1) {
    using namespace fg2;

    DependencyGraph graph;
    Node* n0 = new Node(graph, "node 0");
    Node* n1 = new Node(graph, "node 1");
    Node* n2 = new Node(graph, "node 2");
    Node* n1_0 = new Node(graph, "node 1.0");

    new DependencyGraph::Edge(graph, n0, n1);
    new DependencyGraph::Edge(graph, n1, n2);
    new DependencyGraph::Edge(graph, n1, n1_0);
    n2->makeTarget();

    graph.cull();

    graph.export_graphviz(utils::slog.d);

    EXPECT_TRUE(n1_0->isCulled());
    EXPECT_TRUE(n1_0->isCulledCalled());

    EXPECT_FALSE(n2->isCulled());
    EXPECT_FALSE(n1->isCulled());
    EXPECT_FALSE(n0->isCulled());
    EXPECT_FALSE(n2->isCulledCalled());
    EXPECT_FALSE(n1->isCulledCalled());
    EXPECT_FALSE(n0->isCulledCalled());

    EXPECT_EQ(n0->getRefCount(), 1);
    EXPECT_EQ(n1->getRefCount(), 1);
    EXPECT_EQ(n2->getRefCount(), 1);

    auto edges = graph.getEdges();
    auto nodes = graph.getNodes();
    graph.clear();
    for (auto e : edges) { delete e; }
    for (auto n : nodes) { delete n; }
}

TEST(FrameGraph2Test, GraphCulling2) {
    using namespace fg2;

    DependencyGraph graph;
    Node* n0 = new Node(graph, "node 0");
    Node* n1 = new Node(graph, "node 1");
    Node* n2 = new Node(graph, "node 2");
    Node* n1_0 = new Node(graph, "node 1.0");
    Node* n1_0_0 = new Node(graph, "node 1.0.0");
    Node* n1_0_1 = new Node(graph, "node 1.0.0");

    new DependencyGraph::Edge(graph, n0, n1);
    new DependencyGraph::Edge(graph, n1, n2);
    new DependencyGraph::Edge(graph, n1, n1_0);
    new DependencyGraph::Edge(graph, n1_0, n1_0_0);
    new DependencyGraph::Edge(graph, n1_0, n1_0_1);
    n2->makeTarget();

    graph.cull();

    graph.export_graphviz(utils::slog.d);

    EXPECT_TRUE(n1_0->isCulled());
    EXPECT_TRUE(n1_0_0->isCulled());
    EXPECT_TRUE(n1_0_1->isCulled());
    EXPECT_TRUE(n1_0->isCulledCalled());
    EXPECT_TRUE(n1_0_0->isCulledCalled());
    EXPECT_TRUE(n1_0_1->isCulledCalled());

    EXPECT_FALSE(n2->isCulled());
    EXPECT_FALSE(n1->isCulled());
    EXPECT_FALSE(n0->isCulled());
    EXPECT_FALSE(n2->isCulledCalled());
    EXPECT_FALSE(n1->isCulledCalled());
    EXPECT_FALSE(n0->isCulledCalled());

    EXPECT_EQ(n0->getRefCount(), 1);
    EXPECT_EQ(n1->getRefCount(), 1);
    EXPECT_EQ(n2->getRefCount(), 1);

    auto edges = graph.getEdges();
    auto nodes = graph.getNodes();
    graph.clear();
    for (auto e : edges) { delete e; }
    for (auto n : nodes) { delete n; }
}

TEST(FrameGraph2Test, Simple) {
    using namespace fg2;
    MockResourceAllocator resourceAllocator;
    fg2::FrameGraph fg(resourceAllocator);
    struct Data {
        fg2::FrameGraphId<Texture> output;
    };
    auto& pass0 = fg.addPass<Data>("pass0",
            [&](fg2::FrameGraph::Builder& builder, auto& data) {
                data.output = builder.create<Texture>("Resource0", {.width=16, .height=32});
                data.output = builder.write(data.output, Texture::Usage::UPLOADABLE);
            },
            [=](FrameGraphResources const& resources, auto const& data, backend::DriverApi& driver) {
            });

    auto& pass1 = fg.addPass<Data>("pass1",
            [&](fg2::FrameGraph::Builder& builder, auto& data) {
                builder.read(pass0.getData().output, Texture::Usage::SAMPLEABLE);
                data.output = builder.create<Texture>("Resource1", {.width=32, .height=64});
                data.output = builder.write(data.output, Texture::Usage::COLOR_ATTACHMENT);
            },
            [=](FrameGraphResources const& resources, auto const& data, backend::DriverApi& driver) {
            });

    fg.present(pass1.getData().output);

    fg.compile();
}

TEST_F(FrameGraphTest, FG2Complexe) {
    using namespace fg2;
    MockResourceAllocator resourceAllocator;
    fg2::FrameGraph fg(resourceAllocator);

    struct DepthPassData {
        fg2::FrameGraphId<Texture> depth;
    };
    auto& depthPass = fg.addPass<DepthPassData>("Depth pass",
            [&](fg2::FrameGraph::Builder& builder, auto& data) {
                data.depth = builder.create<Texture>("Depth Buffer", {.width=16, .height=32});
                data.depth = builder.write(data.depth, Texture::Usage::DEPTH_ATTACHMENT);
            },
            [=](fg2::FrameGraphResources const& resources, auto const& data, backend::DriverApi& driver) {
                Texture const& depth = resources.get(data.depth);
                EXPECT_TRUE((bool)depth.texture);
            });

    struct GBufferPassData {
        fg2::FrameGraphId<Texture> depth;
        fg2::FrameGraphId<Texture> gbuf1;
        fg2::FrameGraphId<Texture> gbuf2;
        fg2::FrameGraphId<Texture> gbuf3;
    };
    auto& gBufferPass = fg.addPass<GBufferPassData>("Gbuffer pass",
            [&](fg2::FrameGraph::Builder& builder, auto& data) {
                data.depth = builder.read(depthPass->depth, Texture::Usage::DEPTH_ATTACHMENT);
                Texture::Descriptor desc = builder.getDescriptor(data.depth);
                data.gbuf1 = builder.create<Texture>("Gbuffer 1", desc);
                data.gbuf2 = builder.create<Texture>("Gbuffer 2", desc);
                data.gbuf3 = builder.create<Texture>("Gbuffer 3", desc);

                data.depth = builder.write(data.depth, Texture::Usage::DEPTH_ATTACHMENT);
                data.gbuf1 = builder.write(data.gbuf1, Texture::Usage::COLOR_ATTACHMENT);
                data.gbuf2 = builder.write(data.gbuf2, Texture::Usage::COLOR_ATTACHMENT);
                data.gbuf3 = builder.write(data.gbuf3, Texture::Usage::COLOR_ATTACHMENT);
            },
            [=](FrameGraphResources const& resources, auto const& data, backend::DriverApi& driver) {
                Texture const& depth = resources.get(data.depth);
                Texture const& gbuf1 = resources.get(data.gbuf1);
                Texture const& gbuf2 = resources.get(data.gbuf2);
                Texture const& gbuf3 = resources.get(data.gbuf3);
                EXPECT_TRUE((bool)depth.texture);
                EXPECT_TRUE((bool)gbuf1.texture);
                EXPECT_TRUE((bool)gbuf2.texture);
                EXPECT_TRUE((bool)gbuf3.texture);
            });

    struct LightingPassData {
        fg2::FrameGraphId<Texture> lightingBuffer;
        fg2::FrameGraphId<Texture> depth;
        fg2::FrameGraphId<Texture> gbuf1;
        fg2::FrameGraphId<Texture> gbuf2;
        fg2::FrameGraphId<Texture> gbuf3;
    };
    auto& lightingPass = fg.addPass<LightingPassData>("Lighting pass",
            [&](fg2::FrameGraph::Builder& builder, auto& data) {
                data.depth = builder.read(gBufferPass->depth, Texture::Usage::SAMPLEABLE);
                data.gbuf1 = gBufferPass->gbuf1; //builder.read(gBufferPass->gbuf1, Texture::Usage::SAMPLEABLE);
                data.gbuf2 = builder.read(gBufferPass->gbuf2, Texture::Usage::SAMPLEABLE);
                data.gbuf3 = builder.read(gBufferPass->gbuf3, Texture::Usage::SAMPLEABLE);
                Texture::Descriptor desc = builder.getDescriptor(data.depth);
                data.lightingBuffer = builder.create<Texture>("Lighting buffer", desc);
                data.lightingBuffer = builder.write(data.lightingBuffer, Texture::Usage::COLOR_ATTACHMENT);
            },
            [=](FrameGraphResources const& resources, auto const& data, backend::DriverApi& driver) {
                Texture const& lightingBuffer = resources.get(data.lightingBuffer);
                Texture const& depth = resources.get(data.depth);
                Texture const& gbuf1 = resources.get(data.gbuf1);
                Texture const& gbuf2 = resources.get(data.gbuf2);
                Texture const& gbuf3 = resources.get(data.gbuf3);
                EXPECT_TRUE((bool)lightingBuffer.texture);
                EXPECT_TRUE((bool)depth.texture);
                EXPECT_FALSE((bool)gbuf1.texture);
                EXPECT_TRUE((bool)gbuf2.texture);
                EXPECT_TRUE((bool)gbuf3.texture);
            });

    struct DebugPass {
        fg2::FrameGraphId<Texture> debugBuffer;
        fg2::FrameGraphId<Texture> gbuf1;
        fg2::FrameGraphId<Texture> gbuf2;
        fg2::FrameGraphId<Texture> gbuf3;
    };
    auto& culledPass = fg.addPass<DebugPass>("DebugPass pass",
            [&](fg2::FrameGraph::Builder& builder, auto& data) {
                data.gbuf1 = builder.read(lightingPass->gbuf1, Texture::Usage::SAMPLEABLE);
                data.gbuf2 = builder.read(lightingPass->gbuf2, Texture::Usage::SAMPLEABLE);
                data.gbuf3 = builder.read(lightingPass->gbuf3, Texture::Usage::SAMPLEABLE);
                Texture::Descriptor desc = builder.getDescriptor(data.gbuf1);
                data.debugBuffer = builder.create<Texture>("Debug buffer", desc);
                data.debugBuffer = builder.write(data.debugBuffer, Texture::Usage::COLOR_ATTACHMENT);
            },
            [=](FrameGraphResources const& resources, auto const& data, backend::DriverApi& driver) {
                Texture const& debugBuffer = resources.get(data.debugBuffer);
                Texture const& gbuf1 = resources.get(data.gbuf1);
                Texture const& gbuf2 = resources.get(data.gbuf2);
                Texture const& gbuf3 = resources.get(data.gbuf3);
                EXPECT_FALSE((bool)debugBuffer.texture);
                EXPECT_TRUE((bool)gbuf1.texture);
                EXPECT_TRUE((bool)gbuf2.texture);
                EXPECT_TRUE((bool)gbuf3.texture);
            });

    struct PostPassData {
        fg2::FrameGraphId<Texture> lightingBuffer;
        fg2::FrameGraphId<Texture> backBuffer;
        struct {
            fg2::FrameGraphId<Texture> depth;
            fg2::FrameGraphId<Texture> gbuf1;
            fg2::FrameGraphId<Texture> gbuf2;
            fg2::FrameGraphId<Texture> gbuf3;
        } destroyed;
    };
    auto& postPass = fg.addPass<PostPassData>("Post pass",
            [&](fg2::FrameGraph::Builder& builder, auto& data) {
                data.lightingBuffer = builder.read(lightingPass->lightingBuffer, Texture::Usage::SAMPLEABLE);
                Texture::Descriptor desc = builder.getDescriptor(data.lightingBuffer);
                data.backBuffer = builder.create<Texture>("Backbuffer", desc);
                data.backBuffer = builder.write(data.backBuffer, Texture::Usage::COLOR_ATTACHMENT);

                data.destroyed.depth = lightingPass->depth;
                data.destroyed.gbuf1 = lightingPass->gbuf1;
                data.destroyed.gbuf2 = lightingPass->gbuf2;
                data.destroyed.gbuf3 = lightingPass->gbuf3;
            },
            [=](FrameGraphResources const& resources, auto const& data, backend::DriverApi& driver) {
                Texture const& lightingBuffer = resources.get(data.lightingBuffer);
                Texture const& backBuffer = resources.get(data.backBuffer);
                EXPECT_TRUE((bool)lightingBuffer.texture);
                EXPECT_TRUE((bool)backBuffer.texture);
                EXPECT_FALSE((bool)resources.get(data.destroyed.depth).texture);
                EXPECT_FALSE((bool)resources.get(data.destroyed.gbuf1).texture);
                EXPECT_FALSE((bool)resources.get(data.destroyed.gbuf2).texture);
                EXPECT_FALSE((bool)resources.get(data.destroyed.gbuf3).texture);

                EXPECT_EQ(resources.getUsage(data.lightingBuffer),  Texture::Usage::SAMPLEABLE | Texture::Usage::COLOR_ATTACHMENT);
                EXPECT_EQ(resources.getUsage(data.backBuffer),                                   Texture::Usage::COLOR_ATTACHMENT);
                EXPECT_EQ(resources.getUsage(data.destroyed.depth), Texture::Usage::SAMPLEABLE | Texture::Usage::DEPTH_ATTACHMENT);
                EXPECT_EQ(resources.getUsage(data.destroyed.gbuf1),                              Texture::Usage::COLOR_ATTACHMENT);
                EXPECT_EQ(resources.getUsage(data.destroyed.gbuf2), Texture::Usage::SAMPLEABLE | Texture::Usage::COLOR_ATTACHMENT);
                EXPECT_EQ(resources.getUsage(data.destroyed.gbuf3), Texture::Usage::SAMPLEABLE | Texture::Usage::COLOR_ATTACHMENT);
            });

    fg.present(postPass->backBuffer);

    fg.compile();

    fg.execute(driverApi);
}
