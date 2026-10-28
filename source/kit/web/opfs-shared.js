// opfs-shared.js - linked into web builds with --pre-js, so it runs in the page
// and in every worker before the runtime. WasmFS opens browser-storage files
// with createSyncAccessHandle(), whose default mode locks a file to one page:
// a second window of the same game then cannot open the game files the first
// one holds open (streamed tracks, cars). "readwrite-unsafe" (Chrome 121+)
// lets several pages open the same file; each window keeps its own profile
// folder, so no two pages write the same file. Browsers without the mode get
// the default.
if (typeof FileSystemFileHandle !== "undefined" &&
    FileSystemFileHandle.prototype.createSyncAccessHandle &&
    !FileSystemFileHandle.prototype.createSyncAccessHandle.recompShared) {
  const original = FileSystemFileHandle.prototype.createSyncAccessHandle;
  const shared = function (options) {
    return original.call(this, { mode: "readwrite-unsafe" })
      .catch((error) => error instanceof TypeError ? original.call(this) : Promise.reject(error));
  };
  shared.recompShared = true;
  FileSystemFileHandle.prototype.createSyncAccessHandle = shared;
}
