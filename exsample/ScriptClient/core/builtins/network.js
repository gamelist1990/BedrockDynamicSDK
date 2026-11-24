(function (native) {
  'use strict';
  const defaultTimeoutMs = 10000;
  const defaultMaxBytes = 2 * 1024 * 1024;
  const methods = ['DELETE', 'GET', 'HEAD', 'POST', 'PUT'];
  const HttpRequestMethod = Object.freeze(Object.fromEntries(methods.map(m => [m, m])));
  const stringify = JSON.stringify;
  function invalid(message) { throw Object.assign(new Error(message), { code: 'EINVAL' }); }
  class HttpHeader {
    constructor(key, value) {
      if (typeof key !== 'string' || typeof value !== 'string')
        invalid('HTTP header names and values must be strings');
      this.key = key;
      this.value = value;
    }
  }
  class HttpRequest {
    constructor(uri) {
      this.uri = uri;
      this.method = HttpRequestMethod.GET;
      this.headers = [];
      this.body = '';
      this.timeout = defaultTimeoutMs / 1000;
      this.maxResponseBytes = defaultMaxBytes;
    }
    addHeader(key, value) { this.headers.push(new HttpHeader(key, value)); return this; }
    setBody(body) { this.body = body; return this; }
    setHeaders(headers) { this.headers = headers; return this; }
    setMethod(method) { this.method = method; return this; }
    setTimeout(timeout) { this.timeout = timeout; return this; }
  }
  const internal = Symbol();
  class HttpResponse {
    constructor(key, raw, request) {
      if (key !== internal) invalid('HttpResponse cannot be constructed directly');
      Object.defineProperties(this, {
        status: { value: raw.status, enumerable: true },
        headers: { value: raw.headers.map(pair => new HttpHeader(...pair)), enumerable: true },
        body: { value: raw.text, enumerable: true },
        request: { value: request, enumerable: true },
      });
    }
  }
  async function request(uri, method, headers, body, timeoutMs, maxResponseBytes) {
    if (typeof uri !== 'string' || typeof method !== 'string' ||
        typeof timeoutMs !== 'number' || typeof maxResponseBytes !== 'number' ||
        !Array.isArray(headers) || headers.length > 128)
      invalid('invalid HTTP request configuration');
    for (const pair of headers) {
      if (!Array.isArray(pair) || pair.length !== 2 ||
          typeof pair[0] !== 'string' || typeof pair[1] !== 'string')
        invalid('HTTP headers must have string names and values');
    }
    if (body !== undefined && typeof body !== 'string' &&
        !(body instanceof Uint8Array) && !(body instanceof ArrayBuffer))
      invalid('HTTP body must be a string, Uint8Array or ArrayBuffer');
    return native.request(stringify({
      url: uri, method: method.toUpperCase(), headers, timeoutMs, maxResponseBytes,
    }), body);
  }
  class HttpClient {
    constructor(key) {
      if (key !== internal) invalid('HttpClient cannot be constructed directly');
    }
    async request(config) {
      if (!config || (config.body != null && typeof config.body !== 'string') ||
          (config.timeout != null && typeof config.timeout !== 'number'))
        invalid('expected an HttpRequest with a string body and numeric timeout');
      const headers = config.headers ?? [];
      if (!Array.isArray(headers)) invalid('expected an array of HttpHeader values');
      const raw = await request(config.uri, config.method ?? 'GET',
        headers.map(header => [header?.key, header?.value]), config.body ?? '',
        (config.timeout ?? defaultTimeoutMs / 1000) * 1000, config.maxResponseBytes ?? defaultMaxBytes);
      return new HttpResponse(internal, raw, config);
    }
    get(uri) { return this.request(new HttpRequest(uri)); }
    cancelAll(reason) { native.cancel(reason ?? 'HTTP requests cancelled'); }
  }
  class FetchHeaders {
    #pairs;
    constructor(headers) {
      this.#pairs = new Map();
      for (const [key, value] of headers) {
        const name = key.toLowerCase();
        const old = this.#pairs.get(name);
        this.#pairs.set(name, old === undefined ? value : old + ', ' + value);
      }
    }
    get(name) { return this.#pairs.get(String(name).toLowerCase()) ?? null; }
    has(name) { return this.get(name) !== null; }
    entries() { return this.#pairs.entries(); }
    keys() { return this.#pairs.keys(); }
    values() { return this.#pairs.values(); }
    forEach(callback, self) { this.#pairs.forEach((v, k) => callback.call(self, v, k, this)); }
    [Symbol.iterator]() { return this.entries(); }
  }
  async function fetch(url, options = {}) {
    let headers = options.headers ?? [];
    if (headers instanceof FetchHeaders) headers = [...headers];
    if (!Array.isArray(headers)) {
      if (typeof headers !== 'object') invalid('invalid fetch headers');
      headers = Object.entries(headers);
    }
    const raw = await request(url, options.method ?? 'GET', headers, options.body ?? undefined,
      options.timeoutMs ?? defaultTimeoutMs, options.maxResponseBytes ?? defaultMaxBytes);
    let used = false;
    function consume() {
      if (used) invalid('response body already used');
      used = true;
    }
    return {
      status: raw.status, ok: raw.status >= 200 && raw.status < 300,
      headers: new FetchHeaders(raw.headers),
      get bodyUsed() { return used; },
      async text() { consume(); return raw.text; },
      async json() { consume(); return JSON.parse(raw.text); },
      async arrayBuffer() { consume(); return raw.body; },
    };
  }
  globalThis.fetch = fetch;
  return { HttpRequest, HttpResponse, HttpHeader, HttpRequestMethod, HttpClient,
    http: new HttpClient(internal) };
})
