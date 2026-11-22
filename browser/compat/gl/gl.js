// kr_gl_context for Emscripten's --js-library: WebGL2 on the canvas the host passes to the module factory
// (Module.canvas, an OffscreenCanvas in the page's worker). Under Node there is none and the device stays null.
addToLibrary({
  $krGlContext: 0,
  kr_gl_context__deps: ["$GL", "$krGlContext"],
  kr_gl_context: (width, height) => {
    const canvas = Module["canvas"];
    if (!canvas?.getContext) return 0;
    canvas.width = width;
    canvas.height = height;
    if (!krGlContext) {
      // The client draws into its own targets and the back buffer is blitted in, so the canvas needs no depth.
      krGlContext = GL.createContext(canvas, {
        majorVersion: 2,
        minorVersion: 0,
        alpha: false,
        depth: false,
        stencil: false,
        antialias: false,
        preserveDrawingBuffer: false,
      });
      if (krGlContext) GL.makeContextCurrent(krGlContext);
    }
    return krGlContext;
  },
});
