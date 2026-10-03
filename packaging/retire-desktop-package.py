#!/usr/bin/env python3
"""Withdraw the retired desktop DEBs, dependent installers and CI artifacts."""
import json
import os
import re
from pathlib import Path
import urllib.request

repository = os.environ['GITHUB_REPOSITORY']
assert repository == 'Infiltrator-Projects/Filesystem-Support'
token = os.environ['GH_TOKEN']
current = Path('VERSION').read_text().strip()
retired_versions = {'0.5.4', '0.5.5', '0.5.6'}

def api(path, method='GET'):
    assert path.startswith(f'repos/{repository}/')
    request = urllib.request.Request('https://api.github.com/' + path, method=method,
        headers={'Authorization': 'Bearer ' + token, 'Accept': 'application/vnd.github+json',
                 'X-GitHub-Api-Version': '2022-11-28'})
    with urllib.request.urlopen(request, timeout=45) as response:
        body = response.read()
        return json.loads(body) if body else None

releases = []
page = 1
while True:
    batch = api(f'repos/{repository}/releases?per_page=100&page={page}')
    releases.extend(batch)
    if len(batch) < 100: break
    page += 1
for release in releases:
    versions = set(retired_versions if release['tag_name'].removeprefix('v') in retired_versions else ())
    for asset in release['assets']:
        match = re.fullmatch(r'infiltrator-filesystem-support-udisks_(.+)_amd64\.deb', asset['name'])
        if match:
            assert match[1] != current, 'The current release must contain only the main package'
            versions.add(match[1])
    if not versions: continue
    for asset in release['assets']:
        unwanted = asset['name'] == 'RELEASE_SHA256SUMS.txt'
        for version in versions:
            unwanted |= asset['name'] in (
                f'infiltrator-filesystem-support-udisks_{version}_amd64.deb',
                f'Filesystem-Support-{version}-amd64.deb',
                f'Filesystem-Support-{version}-amd64.run')
        if unwanted:
            api(f"repos/{repository}/releases/assets/{asset['id']}", 'DELETE')
            print(f"Withdrawn {release['tag_name']}: {asset['name']}", flush=True)

artifacts = []
page = 1
while True:
    batch = api(f'repos/{repository}/actions/artifacts?per_page=100&page={page}')['artifacts']
    artifacts.extend(batch)
    if len(batch) < 100: break
    page += 1
for artifact in artifacts:
    if artifact['name'] == 'filesystem-support-desktop-client':
        api(f"repos/{repository}/actions/artifacts/{artifact['id']}", 'DELETE')
        print(f"Deleted retired desktop package artifact {artifact['id']}", flush=True)
print('Retired package release assets and CI artifacts withdrawn', flush=True)
