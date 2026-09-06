// Clay handles showConfiguration and webviewclosed by itself and sends one
// AppMessage key per config item. There is deliberately no hand-written
// message handling here: settings.c folds the six card toggles into its
// bitmask, which is a safer place for that logic than a JS file you cannot
// debug from the watch.
//
// @rebble/clay uses config.json, not config.js, and CloudPebble creates that
// file for you when the dependency is added. The extension is required in the
// require path — since SDK 3.13 these resolve relative to this file, and
// require('./config') will not find a .json.
//
// Project Settings must have JS Handling set to CommonJS-style or neither
// require resolves and the config page silently never opens.

var Clay = require('@rebble/clay');
var clayConfig = require('./config.json');
var clay = new Clay(clayConfig);
