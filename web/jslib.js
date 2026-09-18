// Asyncify glue for the browser build. The native C loop is intentionally
// unchanged; these wrappers let blocking SDL/timer calls yield to the browser.
mergeInto(LibraryManager.library, {
  __wrap_SDL_Delay: function (ms) {
    Asyncify.handleSleep(function (wake) { setTimeout(wake, ms); });
  },
  __wrap_usleep: function (us) {
    Asyncify.handleSleep(function (wake) {
      setTimeout(wake, Math.max(1, Math.round(us / 1000)));
    });
  }
});
