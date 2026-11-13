// Copyright (C) 2026 Execute Software. SPDX-License-Identifier: MIT
// Execute the actual compiled support's JavaScript in bounded fresh contexts.
'use strict';
const { execFileSync } = require('node:child_process');
const vm = require('node:vm');
const assert = require('node:assert/strict');
if (process.argv.length !== 3) throw new Error('usage: node provider_script_tests.cjs shared-tests-executable');
const fixtures = JSON.parse(execFileSync(process.argv[2], ['--script-fixtures'], {
    encoding: 'utf8', timeout: 5000, maxBuffer: 4 * 1024 * 1024,
}));
let checks = 0;
function evaluate(body) {
    return vm.runInNewContext(fixtures.evaluation.replace('/*NILWEBVIEW_BODY*/', () => body), {}, { timeout: 1000 });
}
for (const [body, expected] of [
    ['return null;', 'j:null'], ['return undefined;', 'u:'], ['', 'u:'],
    ['return "\\ud800";', 'j:"\\ud800"'], ['return "\\udfff";', 'j:"\\udfff"'],
    ['return true;', 'j:true'], ['return 7.5;', 'j:7.5'], ['return NaN;', 'j:null'],
    ['return {text:"λ😀", byte:"\\u0000"};', 'j:{"text":"λ😀","byte":"\\u0000"}'],
    ['return [1,undefined,3];', 'j:[1,null,3]'],
    ['return Promise.resolve(1);', 'e:async'], ['return {then(){}};', 'e:async'],
    ['return {get then(){throw Error("bad");}};', 'e:serialization'],
    ['return 1n;', 'e:serialization'], ['return Symbol("x");', 'e:serialization'],
    ['return function(){};', 'e:serialization'], ['const a={};a.self=a;return a;', 'e:serialization'],
    ['return {toJSON(){throw Error("bad");}};', 'e:serialization'],
    ['return "x".repeat(1048577);', 'e:limit'],
]) { assert.equal(evaluate(body), expected); ++checks; }
for (const body of ['throw Error("script failure");', 'return (']) {
    assert.throws(() => evaluate(body)); ++checks;
}
for (const transport of ['webkit', 'chrome']) {
    const messages = [];
    const window = {};
    window.top = window;
    if (transport === 'webkit') window.webkit = { messageHandlers: { nilwebview: { postMessage: value => messages.push(value) } } };
    else window.chrome = { webview: { postMessage: value => messages.push(value) } };
    const sandbox = vm.createContext({ window });
    vm.runInContext(fixtures.message, sandbox, { timeout: 1000 });
    vm.runInContext('window.nilwebview.post({n:1,s:"λ",b:"\\u0000"})', sandbox, { timeout: 1000 });
    assert.deepEqual(messages, ['{"n":1,"s":"λ","b":"\\u0000"}']); ++checks;
    assert.equal(Object.getOwnPropertyDescriptor(window, 'nilwebview').writable, false); ++checks;
    assert.equal(Object.isFrozen(window.nilwebview), true); ++checks;
    for (const body of ['window.nilwebview.post(undefined)', 'window.nilwebview.post(1n)', 'window.nilwebview.post("x".repeat(1048577))']) {
        assert.throws(() => vm.runInContext(body, sandbox, { timeout: 1000 })); ++checks;
    }
    assert.equal(messages.length, 1); ++checks;
}
const child = { top: {} };
vm.runInNewContext(fixtures.message, { window: child }, { timeout: 1000 });
assert.equal(child.nilwebview, undefined); ++checks;
console.log(`${checks} WebView script checks passed`);
