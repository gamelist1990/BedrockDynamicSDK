(function (H) {
  'use strict';
  function fail(code, message) { throw Object.assign(new Error(message), { code }); }
  function path(value) {
    if (typeof value !== 'string') fail('WrongType', 'path must be a relative string');
    return value;
  }
  function encoding(options) {
    const value = typeof options === 'string' ? options : options?.encoding;
    if (value == null) return null;
    if (typeof value !== 'string' || !['utf8', 'utf-8'].includes(value.toLowerCase()))
      fail('Invalid', 'only UTF-8 encoding is supported');
    return 'utf8';
  }
  function defaultFlag(options, expected) {
    if (options?.flag !== undefined && options.flag !== expected)
      fail('Invalid', 'custom filesystem open flags are not supported');
  }
  function write(target, data, options, append) {
    encoding(options);
    defaultFlag(options, append ? 'a' : 'w');
    if (typeof data === 'string')
      return (append ? H.filesAppendText : H.filesWriteText)(path(target), data);
    return (append ? H.filesAppendBytes : H.filesWriteBytes)(path(target), data);
  }
  const fs = {
    readFileSync(target, options) {
      defaultFlag(options, 'r');
      return (encoding(options) ? H.filesReadText : H.filesReadBytes)(path(target));
    },
    writeFileSync(target, data, options) { write(target, data, options, false); },
    appendFileSync(target, data, options) { write(target, data, options, true); },
    existsSync(target) { return H.filesExists(path(target)); },
    readdirSync(target = '.', options) {
      encoding(options);
      const entries = H.filesList(path(target));
      return options?.withFileTypes ? entries.map(({name, directory}) => ({
        name, isDirectory: () => directory, isFile: () => !directory,
      })) : entries.map(entry => entry.name);
    },
    mkdirSync(target, options) { return H.filesMkdir(path(target), options?.recursive === true); },
    rmSync(target, options) {
      try { return H.filesRemove(path(target), options?.recursive === true, true); }
      catch (error) { if (!(options?.force && error.code === 'NotFound')) throw error; }
    },
    renameSync(from, to) { return H.filesRename(path(from), path(to)); },
    statSync(target) {
      const { size, modifiedMs, directory } = H.filesStat(path(target));
      return { size, mtimeMs: modifiedMs, mtime: new Date(modifiedMs),
        isFile: () => !directory, isDirectory: () => directory };
    },
    copyFileSync(from, to, mode = 0) {
      if (mode !== 0 && mode !== 1) fail('Invalid', 'unsupported copyFile mode');
      return H.filesCopy(path(from), path(to), mode === 1);
    },
    constants: Object.freeze({ COPYFILE_EXCL: 1 }),
    promises: {},
  };
  // These wrappers defer to the job queue, but the I/O itself remains synchronous.
  for (const name of ['readFile', 'writeFile', 'appendFile', 'readdir', 'mkdir', 'rm',
                      'rename', 'stat', 'copyFile']) {
    const sync = fs[name + 'Sync'];
    fs.promises[name] = async (...args) => { await undefined; return sync(...args); };
    fs[name] = (...args) => {
      const callback = args.pop();
      if (typeof callback !== 'function') fail('WrongType', 'callback must be a function');
      fs.promises[name](...args).then(value => callback(null, value), error => callback(error));
    };
  }
  return fs;
})
