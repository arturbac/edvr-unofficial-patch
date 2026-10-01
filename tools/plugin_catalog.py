#!/usr/bin/env python3
"""Validate and export EDVR's canonical first-party plugin catalog.

The manifest is deliberately data-only. First-party callbacks and dispatch
state live in C++ registries keyed by the stable manifest index/ID.

Usage:
  python tools/plugin_catalog.py --self-test
  python tools/plugin_catalog.py [--emit-cpp PATH] [--emit-json PATH] [--dry-run]

--dry-run performs validation and reports the output sizes without creating
files or directories.
"""

import copy
import json
import os
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MANIFEST = os.path.join(ROOT, 'src', 'plugins', 'plugin_manifest.json')
EXPECTED_IDS = (
    'temporal-aa', 'cockpit-visuals', 'exposure', 'scanners', 'intro',
    'on-foot-panel', 'comfort', 'performance', 'diagnostics',
)
PROFILES = ('vr', 'flat')
EXPECTED_PROFILES = {
    'temporal-aa': ('vr', 'flat'),
    'cockpit-visuals': ('vr',),
    'exposure': ('vr',),
    'scanners': ('vr',),
    'intro': ('vr',),
    'on-foot-panel': ('vr',),
    'comfort': ('vr',),
    'performance': ('vr',),
    'diagnostics': ('vr', 'flat'),
}
EXPECTED_IMPLEMENTATION = {
    'temporal-aa': 'catalog-only',
    'cockpit-visuals': 'phase1-pilot',
    'exposure': 'catalog-only',
    'scanners': 'catalog-only',
    'intro': 'catalog-only',
    'on-foot-panel': 'catalog-only',
    'comfort': 'catalog-only',
    'performance': 'catalog-only',
    'diagnostics': 'catalog-only',
}
DEFAULT_INSTALL = {
    'vr': frozenset(EXPECTED_IDS[:-1]),
    'flat': frozenset(('temporal-aa',)),
}
RECOMMENDED = {
    'vr': frozenset(EXPECTED_IDS[:-1]),
    'flat': frozenset(('temporal-aa',)),
}
RUNTIME_KINDS = frozenset(('none', 'partial', 'required'))


def load_json(path=MANIFEST):
    with open(path, encoding='utf-8') as f:
        return json.load(f)


def ini_documented_keys(path=None):
    """Read setting names from edvr.ini, including commented template keys."""
    path = path or os.path.join(ROOT, 'edvr.ini')
    keys = set()
    section = ''
    with open(path, encoding='utf-8-sig', errors='replace') as f:
        for raw in f:
            line = raw.strip()
            if line.startswith('[') and ']' in line:
                section = line[1:line.index(']')].strip()
                continue
            body = line[1:].strip() if line.startswith(('#', ';')) else line
            if line.startswith(('#', ';')) and ' ' in body.split('=')[0].strip():
                continue
            if '=' not in body:
                continue
            name = body.split('=', 1)[0].strip()
            if not name or any(ch not in 'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_' for ch in name):
                continue
            keys.add((section + '.' if section else '') + name)
    return keys


def _string_list(value, label, problems, unique=True):
    if not isinstance(value, list) or any(not isinstance(x, str) or not x for x in value):
        problems.append('%s must be an array of non-empty strings' % label)
        return []
    if unique and len(value) != len(set(value)):
        problems.append('%s contains duplicate values' % label)
    return value


def validate_manifest(data, documented_keys=None):
    """Return human-readable errors; validate refs, ownership, and defaults."""
    problems = []
    if not isinstance(data, dict):
        return ['manifest must be a JSON object']
    if type(data.get('schemaVersion')) is not int or data.get('schemaVersion') != 1:
        problems.append('unsupported schemaVersion (expected integer 1)')
    if not isinstance(data.get('description'), str) or not data.get('description'):
        problems.append('description must be a non-empty string')
    if not isinstance(data.get('selectionSemantics'), str) or not data.get('selectionSemantics'):
        problems.append('selectionSemantics must explain design defaults versus current availability')
    profiles = _string_list(data.get('profiles'), 'profiles', problems)
    if tuple(profiles) != PROFILES:
        problems.append('profiles must be exactly %s in canonical order' % (', '.join(PROFILES)))

    core_keys = _string_list(data.get('coreOwnedConfigKeys'), 'coreOwnedConfigKeys', problems)
    plugins = data.get('plugins')
    if not isinstance(plugins, list):
        problems.append('plugins must be an array')
        plugins = []
    ids = []
    by_id = {}
    ownership = {}
    for key in core_keys:
        ownership.setdefault(key, []).append('core')

    for position, plugin in enumerate(plugins):
        label = 'plugins[%d]' % position
        if not isinstance(plugin, dict):
            problems.append('%s must be an object' % label)
            continue
        pid = plugin.get('id')
        if not isinstance(pid, str) or not pid:
            problems.append('%s.id must be a non-empty string' % label)
            continue
        ids.append(pid)
        if pid in by_id:
            problems.append('duplicate plugin id: %s' % pid)
        by_id[pid] = plugin
        if type(plugin.get('index')) is not int or plugin.get('index') != position:
            problems.append('%s.index must equal its canonical array position %d' % (pid, position))
        for field in ('name', 'description', 'costNote'):
            if not isinstance(plugin.get(field), str) or not plugin.get(field):
                problems.append('%s.%s must be a non-empty string' % (pid, field))

        supported = _string_list(plugin.get('profiles'), pid + '.profiles', problems)
        recommended = _string_list(plugin.get('recommendedProfiles'), pid + '.recommendedProfiles', problems)
        defaults = _string_list(plugin.get('defaultInstallProfiles'), pid + '.defaultInstallProfiles', problems)
        expected_profiles = EXPECTED_PROFILES.get(pid)
        if expected_profiles is None:
            problems.append('unknown plugin id: %s' % pid)
        elif tuple(supported) != expected_profiles:
            problems.append('%s.profiles must be %s' % (pid, ', '.join(expected_profiles)))
        status = plugin.get('implementationStatus')
        if status != EXPECTED_IMPLEMENTATION.get(pid):
            problems.append('%s.implementationStatus must be %s' %
                            (pid, EXPECTED_IMPLEMENTATION.get(pid, 'a known implementation phase')))
        selectable = _string_list(plugin.get('selectionAvailableProfiles'), pid + '.selectionAvailableProfiles', problems)
        for profile in selectable:
            if profile not in supported:
                problems.append('%s.selectionAvailableProfiles references unsupported profile %s' % (pid, profile))
        if selectable:
            problems.append('%s selection is not available during Phase 1' % pid)
        for field, values in (('profiles', supported), ('recommendedProfiles', recommended),
                              ('defaultInstallProfiles', defaults)):
            for profile in values:
                if profile not in profiles:
                    problems.append('%s.%s references unknown profile %s' % (pid, field, profile))
                elif profile not in supported:
                    problems.append('%s.%s references unsupported profile %s' % (pid, field, profile))
        if not set(recommended).issubset(set(defaults)):
            problems.append('%s recommendedProfiles must be a subset of defaultInstallProfiles' % pid)
        if pid in DEFAULT_INSTALL:
            expected_defaults = tuple(p for p in PROFILES if pid in DEFAULT_INSTALL[p])
            if set(defaults) != set(expected_defaults):
                problems.append('%s.defaultInstallProfiles must be %s' %
                                (pid, ', '.join(expected_defaults) or '(empty)'))
            expected_recommended = tuple(p for p in PROFILES if pid in RECOMMENDED[p])
            if set(recommended) != set(expected_recommended):
                problems.append('%s.recommendedProfiles must be %s' %
                                (pid, ', '.join(expected_recommended) or '(empty)'))

        runtime = plugin.get('runtimeRequirement')
        if not isinstance(runtime, dict) or set(runtime) != set(PROFILES):
            problems.append('%s.runtimeRequirement must define vr and flat' % pid)
            runtime = {}
        for profile in PROFILES:
            value = runtime.get(profile)
            if value not in RUNTIME_KINDS:
                problems.append('%s.runtimeRequirement.%s must be none, partial, or required' % (pid, profile))
            if profile not in supported and value != 'none':
                problems.append('%s has a runtime requirement for unsupported profile %s' % (pid, profile))
            if profile == 'flat' and value != 'none':
                problems.append('%s flat runtimeRequirement must be none' % pid)

        owned = _string_list(plugin.get('ownedConfigKeys'), pid + '.ownedConfigKeys', problems)
        for key in owned:
            ownership.setdefault(key, []).append(pid)
        for field in ('hookPoints', 'rigs'):
            _string_list(plugin.get(field), pid + '.' + field, problems)
        claims = plugin.get('claims')
        if not isinstance(claims, list):
            problems.append(pid + '.claims must be an array')
            claims = []
        claim_ids = set()
        for ci, claim in enumerate(claims):
            clabel = '%s.claims[%d]' % (pid, ci)
            if not isinstance(claim, dict):
                problems.append(clabel + ' must be an object')
                continue
            cid = claim.get('id')
            if not isinstance(cid, str) or not cid:
                problems.append(clabel + '.id must be a non-empty string')
            elif cid in claim_ids:
                problems.append('%s has duplicate claim id %s' % (pid, cid))
            else:
                claim_ids.add(cid)
            if not isinstance(claim.get('hook'), str) or not claim.get('hook'):
                problems.append(clabel + '.hook must be a non-empty string')
            if type(claim.get('precedence')) is not int:
                problems.append(clabel + '.precedence must be an integer')
            if not isinstance(claim.get('rig'), str):
                problems.append(clabel + '.rig must be a string')
            if not isinstance(claim.get('shape'), str):
                problems.append(clabel + '.shape must be a string')
            draw_shape = claim.get('drawShape')
            if not isinstance(draw_shape, dict) or set(draw_shape) != {'kind', 'count', 'instances'}:
                problems.append(clabel + '.drawShape must define exactly kind, count, and instances')
            else:
                kind = draw_shape.get('kind')
                count = draw_shape.get('count')
                instances = draw_shape.get('instances')
                if not isinstance(kind, str) or len(kind) != 1 or kind not in ('D', 'I', 'N', 'X'):
                    problems.append(clabel + '.drawShape.kind must be one of D, I, N, X')
                if type(count) is not int or count < 0 or count > 0xffffffff:
                    problems.append(clabel + '.drawShape.count must be an unsigned 32-bit integer')
                if type(instances) is not int or instances < 1 or instances > 0xffffffff:
                    problems.append(clabel + '.drawShape.instances must be a positive 32-bit integer')
            for relation in ('precedenceAfter', 'precedenceBefore'):
                if relation in claim and (not isinstance(claim[relation], str) or not claim[relation]):
                    problems.append(clabel + '.' + relation + ' must be a non-empty string')
            shader_pairs = claim.get('shaderPairs')
            if not isinstance(shader_pairs, list):
                problems.append(clabel + '.shaderPairs must be an array')
            else:
                seen_pairs = set()
                for si, pair in enumerate(shader_pairs):
                    if not isinstance(pair, dict) or not isinstance(pair.get('vs'), str) or not isinstance(pair.get('ps'), str):
                        problems.append('%s.shaderPairs[%d] must define string vs and ps hashes' % (clabel, si))
                        continue
                    signature = (pair['vs'].upper(), pair['ps'].upper())
                    if len(signature[0]) != 16 or len(signature[1]) != 16 or any(
                            ch not in '0123456789ABCDEF' for value in signature for ch in value):
                        problems.append('%s.shaderPairs[%d] hashes must be 16 hex digits' % (clabel, si))
                    if signature in seen_pairs:
                        problems.append('%s has duplicate shader pair %s/%s' % (pid, pair['vs'], pair['ps']))
                    seen_pairs.add(signature)

        deps = plugin.get('dependencies')
        if not isinstance(deps, list):
            problems.append(pid + '.dependencies must be an array')
            deps = []
        seen_deps = set()
        for di, dep in enumerate(deps):
            dlabel = '%s.dependencies[%d]' % (pid, di)
            if not isinstance(dep, dict):
                problems.append(dlabel + ' must be an object')
                continue
            did, kind = dep.get('id'), dep.get('kind')
            if not isinstance(did, str) or not did:
                problems.append(dlabel + '.id must be a non-empty string')
                continue
            if did == pid:
                problems.append('%s cannot depend on itself' % pid)
            if did in seen_deps:
                problems.append('%s has duplicate dependency %s' % (pid, did))
            seen_deps.add(did)
            if kind not in ('hard', 'soft'):
                problems.append(dlabel + '.kind must be hard or soft')
            if not isinstance(dep.get('reason'), str) or not dep.get('reason'):
                problems.append(dlabel + '.reason must be a non-empty string')

        conflicts = plugin.get('conflicts')
        if not isinstance(conflicts, list):
            problems.append(pid + '.conflicts must be an array')
            conflicts = []
        seen_conflicts = set()
        for ci, conflict in enumerate(conflicts):
            clabel = '%s.conflicts[%d]' % (pid, ci)
            if not isinstance(conflict, dict):
                problems.append(clabel + ' must be an object')
                continue
            cid, kind = conflict.get('id'), conflict.get('kind')
            if not isinstance(cid, str) or not cid:
                problems.append(clabel + '.id must be a non-empty string')
            elif cid == pid:
                problems.append('%s cannot conflict with itself' % pid)
            elif cid in seen_conflicts:
                problems.append('%s has duplicate conflict %s' % (pid, cid))
            else:
                seen_conflicts.add(cid)
            if kind not in ('hard', 'soft'):
                problems.append(clabel + '.kind must be hard or soft')

    if tuple(ids) != EXPECTED_IDS:
        problems.append('plugin IDs/order must be exactly %s' % ', '.join(EXPECTED_IDS))
    pilot = by_id.get('cockpit-visuals', {})
    expected_pilot_claim = {
        'id': 'night-vision',
        'hook': 'draw.classify',
        'precedence': 100,
        'precedenceAfter': 'census.skip-ranges',
        'precedenceBefore': 'legacy.remlok',
        'rig': 'night_vision_test',
        'shaderPairs': [{'vs': 'FCF7BD2896751D96', 'ps': 'F786D34B5E118D5E'}],
        'drawShape': {'kind': 'X', 'count': 240, 'instances': 1},
    }
    pilot_claims = pilot.get('claims', []) if isinstance(pilot, dict) else []
    if not any(all(claim.get(key) == value for key, value in expected_pilot_claim.items())
               for claim in pilot_claims if isinstance(claim, dict)):
        problems.append('cockpit-visuals must preserve the Phase 1 night-vision claim metadata')
    pilot_rigs = pilot.get('rigs', []) if isinstance(pilot, dict) else []
    if not all(rig in pilot_rigs for rig in ('night_vision_test', 'plugin_dispatch_test')):
        problems.append('cockpit-visuals rigs must name night_vision_test and plugin_dispatch_test')
    id_set = set(ids)
    graph = {pid: [] for pid in ids}
    for pid, plugin in by_id.items():
        for dep in plugin.get('dependencies', []) if isinstance(plugin.get('dependencies'), list) else []:
            if not isinstance(dep, dict):
                continue
            did = dep.get('id')
            if did not in id_set:
                problems.append('%s dependency references unknown plugin %s' % (pid, did))
            else:
                graph[pid].append(did)
                if dep.get('kind') == 'hard':
                    dependent_defaults = set(plugin.get('defaultInstallProfiles', []))
                    dependency_defaults = set(by_id.get(did, {}).get('defaultInstallProfiles', []))
                    if not dependent_defaults.issubset(dependency_defaults):
                        problems.append('%s hard dependency %s is absent from one of its target default profiles' %
                                        (pid, did))
                    selectable = plugin.get('selectionAvailableProfiles', [])
                    dependent_available = set(selectable) if isinstance(selectable, list) else set()
                    dependency = by_id.get(did, {})
                    dep_selectable = dependency.get('selectionAvailableProfiles', [])
                    dependency_available = set(dep_selectable) if isinstance(dep_selectable, list) else set()
                    dep_profiles = dependency.get('profiles', [])
                    dependency_profiles = set(dep_profiles) if isinstance(dep_profiles, list) else set()
                    if not dependent_available.issubset(dependency_profiles):
                        problems.append('%s hard dependency %s is unsupported in a selectable profile' %
                                        (pid, did))
                    if not dependent_available.issubset(dependency_available):
                        problems.append('%s hard dependency %s is unavailable in a selectable profile' %
                                        (pid, did))
        for conflict in plugin.get('conflicts', []) if isinstance(plugin.get('conflicts'), list) else []:
            if isinstance(conflict, dict) and conflict.get('id') not in id_set:
                problems.append('%s conflict references unknown plugin %s' % (pid, conflict.get('id')))
    visiting, visited = set(), set()

    def visit(pid):
        if pid in visiting:
            problems.append('dependency cycle includes %s' % pid)
            return
        if pid in visited:
            return
        visiting.add(pid)
        for child in graph.get(pid, ()):
            visit(child)
        visiting.remove(pid)
        visited.add(pid)

    for pid in graph:
        visit(pid)

    for key, owners in sorted(ownership.items()):
        if len(owners) > 1:
            problems.append('duplicate config ownership for %s: %s' % (key, ', '.join(owners)))
    if documented_keys is not None:
        documented = set(documented_keys)
        for key in sorted(set(ownership) - documented):
            problems.append('config ownership references unknown key: %s' % key)
        for key in sorted(documented - set(ownership)):
            problems.append('config key has no owner: %s' % key)
    return problems


def profile_mask(profile_names):
    return sum(1 << PROFILES.index(name) for name in profile_names)


def _cpp_string(value):
    return json.dumps(value, ensure_ascii=True)


def render_cpp(data):
    """Render the static, allocation-free C-compatible metadata tables."""
    lines = [
        '// Generated by tools/plugin_catalog.py -- do not edit.',
        '// Array position and index are the stable first-party plugin ID/mask bit.',
        '#pragma once',
        '#include <cstdint>',
        'namespace edvr { namespace plugins {',
        'enum ProfileMask : std::uint32_t {',
        '    kProfileNone = 0,',
        '    kProfileVr = 1u << 0,',
        '    kProfileFlat = 1u << 1,',
        '    kProfileLegacyVr = kProfileVr,',
        '};',
        'struct Dependency { const char* id; std::uint32_t kind; const char* reason; };',
        'struct ShaderPair { std::uint64_t vertexShaderHash, pixelShaderHash; };',
        'struct DrawShape { std::uint8_t kind; std::uint32_t count, instances; };',
        'struct Claim { const char* id; const char* hook; std::int32_t precedence; const char* precedenceAfter; const char* precedenceBefore; const char* rig; const char* shape; DrawShape drawShape; const ShaderPair* shaderPairs; std::uint32_t shaderPairCount; };',
        'struct PluginRecord {',
        '    std::uint32_t index;',
        '    const char* id; const char* name; const char* description;',
        '    const char* implementationStatus;',
        '    // Target architecture defaults; use selectionAvailableProfileMask for current availability.',
        '    std::uint32_t profileMask, recommendedProfileMask, defaultInstallProfileMask;',
        '    std::uint32_t selectionAvailableProfileMask;',
        '    std::uint32_t runtimeRequiredProfileMask, runtimePartialProfileMask;',
        '    const char* const* ownedConfigKeys; std::uint32_t ownedConfigKeyCount;',
        '    const char* const* hookPoints; std::uint32_t hookPointCount;',
        '    const Claim* claims; std::uint32_t claimCount;',
        '    const Dependency* dependencies; std::uint32_t dependencyCount;',
        '    const char* const* rigs; std::uint32_t rigCount;',
        '    const char* costNote;',
        '};',
    ]
    lines.append('inline constexpr const char* kCoreOwnedConfigKeys[] = {')
    lines.extend('    %s,' % _cpp_string(value) for value in data['coreOwnedConfigKeys'])
    lines.append('};')
    lines.append('inline constexpr std::uint32_t kCoreOwnedConfigKeyCount = %d;' %
                 len(data['coreOwnedConfigKeys']))
    for plugin in data['plugins']:
        pid = plugin['id'].replace('-', '_')
        for ci, claim in enumerate(plugin['claims']):
            pairs = claim['shaderPairs']
            lines.append('inline constexpr ShaderPair k_%s_claim_%d_shader_pairs[] = {' % (pid, ci))
            for pair in pairs:
                lines.append('    {0x%sull, 0x%sull},' % (pair['vs'], pair['ps']))
            if not pairs:
                lines.append('    {0ull, 0ull},')
            lines.append('};')
            claim_symbol = ''.join(part.capitalize() for part in claim['id'].replace('_', '-').split('-'))
            plugin_symbol = ''.join(part.capitalize() for part in plugin['id'].replace('_', '-').split('-'))
            lines.append('enum : std::uint32_t { kClaim%s%s = %d };' %
                         (plugin_symbol, claim_symbol, ci))
        for field, key in (('owned_config_keys', 'ownedConfigKeys'), ('hook_points', 'hookPoints'), ('rigs', 'rigs')):
            values = plugin[key]
            lines.append('inline constexpr const char* k_%s_%s[] = {' % (pid, field))
            lines.extend('    %s,' % _cpp_string(value) for value in values)
            if not values:
                lines.append('    "",')
            lines.append('};')
        lines.append('inline constexpr Claim k_%s_claims[] = {' % pid)
        for ci, claim in enumerate(plugin['claims']):
            shape = claim['drawShape']
            lines.append('    {%s, %s, %d, %s, %s, %s, %s, {0x%02Xu, %du, %du}, k_%s_claim_%d_shader_pairs, %d},' % (
                _cpp_string(claim['id']), _cpp_string(claim['hook']), claim['precedence'],
                _cpp_string(claim.get('precedenceAfter', '')), _cpp_string(claim.get('precedenceBefore', '')),
                _cpp_string(claim['rig']), _cpp_string(claim['shape']), ord(shape['kind']),
                shape['count'], shape['instances'], pid, ci,
                len(claim['shaderPairs'])))
        if not plugin['claims']:
            lines.append('    {"", "", 0, "", "", "", "", {0, 0, 0}, nullptr, 0},')
        lines.append('};')
        lines.append('inline constexpr Dependency k_%s_dependencies[] = {' % pid)
        for dep in plugin['dependencies']:
            lines.append('    {%s, %d, %s},' % (
                _cpp_string(dep['id']), 1 if dep['kind'] == 'hard' else 2, _cpp_string(dep['reason'])))
        if not plugin['dependencies']:
            lines.append('    {"", 0, ""},')
        lines.append('};')
    lines.append('inline constexpr PluginRecord kManifest[] = {')
    for plugin in data['plugins']:
        pid = plugin['id'].replace('-', '_')
        recommended = profile_mask(plugin['recommendedProfiles'])
        defaults = profile_mask(plugin['defaultInstallProfiles'])
        required = profile_mask([p for p, kind in plugin['runtimeRequirement'].items() if kind == 'required'])
        partial = profile_mask([p for p, kind in plugin['runtimeRequirement'].items() if kind == 'partial'])
        lines.append('    {%d, %s, %s, %s, %s, 0x%xu, 0x%xu, 0x%xu, 0x%xu, 0x%xu, 0x%xu,' % (
            plugin['index'], _cpp_string(plugin['id']), _cpp_string(plugin['name']),
            _cpp_string(plugin['description']), _cpp_string(plugin['implementationStatus']),
            profile_mask(plugin['profiles']), recommended, defaults,
            profile_mask(plugin['selectionAvailableProfiles']), required, partial))
        lines.append('     k_%s_owned_config_keys, %d, k_%s_hook_points, %d,' %
                     (pid, len(plugin['ownedConfigKeys']), pid, len(plugin['hookPoints'])))
        lines.append('     k_%s_claims, %d, k_%s_dependencies, %d,' %
                     (pid, len(plugin['claims']), pid, len(plugin['dependencies'])))
        lines.append('     k_%s_rigs, %d, %s},' % (pid, len(plugin['rigs']), _cpp_string(plugin['costNote'])))
    index_names = [plugin['id'].replace('-', '_').title().replace('_', '') for plugin in data['plugins']]
    lines.append('};')
    lines.append('enum PluginIndex : std::uint32_t {')
    lines.extend('    kPlugin%s = %d,' % (index_names[i], i) for i in range(len(index_names)))
    lines.append('    kPluginIndexCount = %d' % len(index_names))
    lines.extend(['};', 'inline constexpr std::uint32_t kPluginCount = %d;' % len(data['plugins']),
                  'inline constexpr std::uint32_t kVrProfileBit = 0x1u;',
                  'inline constexpr std::uint32_t kFlatProfileBit = 0x2u;',
                  '}}  // namespace edvr::plugins', ''])
    return '\n'.join(lines)


def _write_or_report(path, content, dry_run):
    if dry_run:
        print('[edvr] dry run: %s (%d bytes) not written' % (path, len(content.encode('utf-8'))))
        return
    parent = os.path.dirname(os.path.abspath(path))
    os.makedirs(parent, exist_ok=True)
    with open(path, 'w', encoding='utf-8', newline='\n') as f:
        f.write(content)
    print('[edvr] wrote %s' % path)


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    if '--self-test' in argv:
        return self_test()
    emit_cpp = None
    emit_json = None
    dry_run = '--dry-run' in argv
    for i, arg in enumerate(argv):
        if arg == '--emit-cpp' and i + 1 < len(argv):
            emit_cpp = argv[i + 1]
        elif arg == '--emit-json' and i + 1 < len(argv):
            emit_json = argv[i + 1]
    if any(arg in ('--emit-cpp', '--emit-json') for arg in argv) and not (emit_cpp or emit_json):
        print('plugin_catalog: --emit-cpp and --emit-json require a path')
        return 2
    try:
        data = load_json()
        problems = validate_manifest(data, ini_documented_keys())
    except (OSError, ValueError, TypeError) as exc:
        print('plugin_catalog: cannot load catalog: %s' % exc)
        return 1
    if problems:
        print('plugin_catalog: validation FAILED')
        for problem in problems:
            print('  ' + problem)
        return 1
    if emit_cpp:
        _write_or_report(emit_cpp, render_cpp(data), dry_run)
    if emit_json:
        _write_or_report(emit_json, json.dumps(data, indent=2, ensure_ascii=False) + '\n', dry_run)
    if not emit_cpp and not emit_json:
        print('[edvr] plugin catalog ok: %d plugins, %d owned config keys' %
              (len(data['plugins']), len(ini_documented_keys())))
    return 0


def self_test():
    failures = []

    def check(name, condition):
        if not condition:
            failures.append(name)

    try:
        original = load_json()
        documented = ini_documented_keys()
        check('canonical manifest validates', not validate_manifest(original, documented))

        bad = copy.deepcopy(original)
        bad['plugins'][1]['id'] = bad['plugins'][0]['id']
        check('duplicate plugin ID rejected', any('duplicate plugin id' in p for p in validate_manifest(bad, documented)))
        bad = copy.deepcopy(original)
        bad['plugins'][0]['ownedConfigKeys'][0] = 'fix.no_such_setting'
        check('unknown key reference rejected', any('unknown key' in p for p in validate_manifest(bad, documented)))
        bad = copy.deepcopy(original)
        bad['plugins'][1]['ownedConfigKeys'].append(bad['plugins'][0]['ownedConfigKeys'][0])
        check('duplicate key ownership rejected', any('duplicate config ownership' in p for p in validate_manifest(bad, documented)))
        bad = copy.deepcopy(original)
        bad['plugins'][0]['dependencies'].append({'id': 'ghost', 'kind': 'hard', 'reason': 'fixture'})
        check('unknown dependency rejected', any('unknown plugin' in p for p in validate_manifest(bad, documented)))
        bad = copy.deepcopy(original)
        bad['plugins'][0]['dependencies'].append({'id': 'cockpit-visuals', 'kind': 'hard', 'reason': 'fixture'})
        bad['plugins'][1]['dependencies'].append({'id': 'temporal-aa', 'kind': 'soft', 'reason': 'fixture'})
        check('dependency cycle rejected', any('dependency cycle' in p for p in validate_manifest(bad, documented)))
        bad = copy.deepcopy(original)
        bad['plugins'][1]['conflicts'].append({'id': 'cockpit-visuals', 'kind': 'hard'})
        check('self conflict rejected', any('cannot conflict with itself' in p for p in validate_manifest(bad, documented)))
        bad = copy.deepcopy(original)
        bad['plugins'][1]['conflicts'] = [
            {'id': 'temporal-aa', 'kind': 'soft'}, {'id': 'temporal-aa', 'kind': 'hard'}]
        check('duplicate conflict rejected', any('duplicate conflict' in p for p in validate_manifest(bad, documented)))
        bad = copy.deepcopy(original)
        bad['plugins'][5]['dependencies'][0]['kind'] = 'hard'
        bad['plugins'][5]['selectionAvailableProfiles'] = ['vr']
        check('hard dependency availability checked for selectable profiles',
              any('hard dependency temporal-aa is unavailable in a selectable profile' in p
                  for p in validate_manifest(bad, documented)))
        bad = copy.deepcopy(original)
        bad['plugins'][0]['defaultInstallProfiles'] = ['bogus']
        check('invalid default profile rejected', any('unsupported profile' in p or 'unknown profile' in p for p in validate_manifest(bad, documented)))
        bad = copy.deepcopy(original)
        bad['coreOwnedConfigKeys'].pop()
        check('missing ownership rejected', any('has no owner' in p for p in validate_manifest(bad, documented)))
        bad = copy.deepcopy(original)
        bad['schemaVersion'] = True
        check('boolean schema version rejected', any('schemaVersion' in p for p in validate_manifest(bad, documented)))
        bad = copy.deepcopy(original)
        bad['plugins'][1]['claims'][0]['drawShape']['kind'] = 'Z'
        check('unknown draw kind rejected', any('drawShape.kind' in p for p in validate_manifest(bad, documented)))
        bad = copy.deepcopy(original)
        bad['plugins'][1]['claims'][0]['drawShape']['count'] = True
        check('boolean draw count rejected', any('drawShape.count' in p for p in validate_manifest(bad, documented)))

        with tempfile.TemporaryDirectory(prefix='edvr-plugin-catalog-') as td:
            target = os.path.join(td, 'missing', 'manifest.inc')
            content = render_cpp(original)
            _write_or_report(target, content, True)
            check('dry run creates no directory', not os.path.exists(os.path.dirname(target)))
        check('generated metadata has stable indices', '0, "temporal-aa"' in content and
                  '8, "diagnostics"' in content and 'kPluginIndexCount = 9' in content and
                  '0xFCF7BD2896751D96ull' in content and
                  'kCoreOwnedConfigKeyCount' in content and
                  'kPluginTemporalAa = 0' in content and
                  'kClaimCockpitVisualsNightVision = 0' in content and
                  '{0x58u, 240u, 1u}' in content)
    except Exception as exc:
        failures.append('unexpected exception: %s' % exc)

    if failures:
        print('plugin_catalog: self-test FAILED')
        for failure in failures:
            print('  ' + failure)
        return 1
    print('plugin_catalog: self-test OK')
    return 0


if __name__ == '__main__':
    sys.exit(main())
