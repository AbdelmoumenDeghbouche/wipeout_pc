"""Compile selected shadPS4/GoldHEN XML patches into out/patches.bin for the loader.

Patch addresses are PS4 virtual addresses (eboot base 0x400000); the loader's
image places eboot vaddr 0 at image offset 0. Only literal writes are supported
(bytes, bytes16/32/64, float32/64, utf8, utf16); pattern ("mask") patches are rejected.
The built-in file's lines carry signatures (Pattern/At) that find each site in any build.
"""
import argparse
import copy
import hashlib
import json
import os
import re
import struct
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

EBOOT_BASE=0x400000


def game_app_version(game):
    """APP_VER of the game folder's param.sfo ("01.02"), or None when it cannot be read."""
    return game_sfo(game).get('APP_VER')


def eboot_segments(elf):
    phoff,=struct.unpack_from('<Q',elf,0x20)
    phentsize,phnum=struct.unpack_from('<HH',elf,0x36)
    segments=[]
    for i in range(phnum):
        kind,_,_,vaddr,_,_,memsz,_=struct.unpack_from('<IIQQQQQQ',elf,phoff+i*phentsize)
        if kind==1: segments.append((vaddr,vaddr+memsz))
    return segments


def eboot_image(elf):
    """The PT_LOAD file bytes of eboot.elf at their virtual addresses (image offsets)."""
    phoff,=struct.unpack_from('<Q',elf,0x20)
    phentsize,phnum=struct.unpack_from('<HH',elf,0x36)
    loads=[struct.unpack_from('<IIQQQQQQ',elf,phoff+i*phentsize) for i in range(phnum)]
    loads=[(vaddr,offset,filesz) for kind,_,offset,vaddr,_,filesz,_,_ in loads if kind==1]
    image=bytearray(max(vaddr+filesz for vaddr,_,filesz in loads))
    for vaddr,offset,filesz in loads: image[vaddr:vaddr+filesz]=elf[offset:offset+filesz]
    return bytes(image)


# Signatures find a patch site in any build whose code matches, wherever the code moved: the
# original bytes around the site, "??" where a build may differ (rip-relative displacements,
# rel32 branch targets). tools/make_signatures.py writes them from the reference build.
def parse_pattern(text):
    return [None if b=='??' else int(b,16) for b in text.split()]


def find_pattern(image, pattern, limit=2):
    """Image offsets where pattern matches, at most limit of them."""
    runs,start=[],None
    for i,b in enumerate(pattern+[None]):
        if b is not None and start is None: start=i
        if b is None and start is not None: runs.append((start,i)); start=None
    if not runs: raise ValueError('pattern without fixed bytes')
    a,b=max(runs,key=lambda r:r[1]-r[0])
    anchor=bytes(pattern[a:b])
    found,at=[],image.find(anchor)
    while at>=0 and len(found)<limit:
        s=at-a
        if s>=0 and s+len(pattern)<=len(image) and all(p is None or image[s+i]==p for i,p in enumerate(pattern)):
            found.append(s)
        at=image.find(anchor,at+1)
    return found


def locate(image, element, prefix='', *, allow_address=False, validate_only=False):
    """Resolve a unique signature; absolute addresses require an explicit verified-build policy."""
    pattern=element.get(prefix+'Pattern')
    if pattern and (image is not None or validate_only):
        parsed=parse_pattern(pattern)
        if not any(b is not None for b in parsed):
            raise ValueError('pattern without fixed bytes')
        at=int(element.get(prefix+'At','0'),0)
        if validate_only:
            return 0
        found=find_pattern(image,parsed)
        if len(found)!=1:
            raise LookupError('signature not found' if not found else 'signature found more than once')
        return found[0]+at
    if not allow_address:
        raise LookupError('absolute Address requires the verified CUSA05670 / APP_VER 01.07 eboot.bin')
    return int(element.get('Address') or '',0)-EBOOT_BASE


def resolve_targets(image, meta, *, allow_address=False, segments=None, validate_only=False):
    """Resolve only effective Line/Target/rel dependencies, using the existing XML syntax."""
    lines=list(meta.iter('Line'))
    definitions={}
    for target in meta.iter('Target'):
        definitions.setdefault(target.get('Name'),[]).append(target)
    sites,targets,visiting={},{},set()

    def resolve(kind, key):
        cache=sites if kind=='Line' else targets
        if key in cache:
            return cache[key]
        node=(kind,key)
        if node in visiting:
            raise LookupError(f'cyclic {kind} dependency {key}')
        visiting.add(node)
        if kind=='Line':
            if not 0<=key<len(lines):
                raise LookupError(f'Target Line {key} is outside the patch')
            element=lines[key]
            at=resolve('Target',element.get('Target')) if element.get('Target') else locate(
                image,element,allow_address=allow_address,validate_only=validate_only)
        else:
            choices=definitions.get(key,[])
            if len(choices)!=1:
                raise LookupError(f'Target {key!r} is missing or duplicated')
            element=choices[0]
            if element.get('Target'):
                raise LookupError('Target-to-Target attributes are unsupported; use the existing Line reference')
            if element.get('Line') is not None:
                at=resolve('Line',int(element.get('Line')))
            else:
                at=locate(image,element,allow_address=allow_address,validate_only=validate_only)
                if element.get('Ref'):
                    int(element.get('Ref'),0)
                    tail=int(element.get('Tail','0'),0)
                    if not validate_only and image is not None:
                        if not 0<=at<=len(image)-4:
                            raise LookupError('relative Target operand is outside the eboot')
                        disp,=struct.unpack_from('<i',image,at)
                        at=at+4+tail+disp
            if not validate_only and segments is not None and not any(start<=at<end for start,end in segments):
                raise LookupError(f'Target {key!r} is outside the eboot')
        visiting.remove(node)
        cache[key]=at
        return at

    for index,line in enumerate(lines):
        resolve('Line',index)
        for name in re.findall(r'\{rel:([^:}]+):[^}]+\}',line.get('Value','')):
            resolve('Target',name)
    for table in meta.iter('Table'):
        resolve('Target',table.get('Target'))
    return [sites[i] for i in range(len(lines))],targets


def fill(value, at, targets):
    """Value bytes with each {rel:NAME:TAIL} replaced by the rel32 to target NAME, measured from the
    end of the instruction (TAIL bytes after the operand)."""
    out=bytearray()
    for part in re.split(r'(\{[^}]*\})',value.replace(' ','')):
        if not part.startswith('{'):
            out+=bytes.fromhex(part)
            continue
        kind,name,tail=part[1:-1].split(':')
        if kind!='rel':
            raise ValueError(f'unsupported placeholder {kind!r}')
        out+=struct.pack('<i',targets[name]-(at+len(out)+4+int(tail)))
    return bytes(out)


def encode(line):
    kind,value=line.get('Type'),line.get('Value')
    if value is None:
        raise ValueError('missing patch Value')
    if kind=='bytes': return bytes.fromhex(value.replace(' ',''))
    if kind in ('bytes16','bytes32','bytes64'):
        return int(value,0).to_bytes(int(kind[5:])//8,'little')
    if kind=='float32': return struct.pack('<f',float(value))
    if kind=='float64': return struct.pack('<d',float(value))
    if kind=='utf8': return value.encode()+b'\0'
    if kind=='utf16': return value.encode('utf-16-le')+b'\0\0'
    raise ValueError(f'unsupported patch type {kind!r}')


# RIP-relative LEA (REX.W, any register), followed by its signed 32-bit operand.
LEA_RIP=re.compile(rb'[\x48\x4c]\x8d[\x05\x0d\x15\x1d\x25\x2d\x35\x3d]')


def move_table(image, table, old, reserve, segments, *, validate_only=False):
    size,entry,count,refs=(int(table.get(k),0) for k in ('Size','Entry','Count','Refs'))
    needed=(count+1)*entry
    if min(size,entry,count,refs)<=0 or size%entry or count*entry<size or needed>(1<<63)-1:
        raise ValueError('invalid table layout')
    if validate_only:
        return []
    if not reserve or len(reserve)!=2 or reserve[1]<needed:
        raise LookupError('no memory reserved for the bigger table')
    base,capacity=reserve
    if base<0 or capacity<=0 or base+capacity>(1<<63)-1:
        raise ValueError('invalid table reserve')
    if any(base<end and start<base+capacity for start,end in segments):
        raise ValueError('table reserve overlaps an eboot segment')
    if old<0 or not any(start<=old and old+size<=end for start,end in segments):
        raise ValueError('source table is outside the eboot segments')
    writes=[]
    for m in LEA_RIP.finditer(image):
        at=m.end()
        if at+4>len(image):
            continue
        target=at+4+struct.unpack_from('<i',image,at)[0]
        if old<=target<=old+size:
            if not any(start<=m.start() and at+4<=end for start,end in segments):
                raise ValueError('table reference is outside the eboot segments')
            moved=base+entry+(target-old if target<old+size else count*entry)
            writes.append((at,struct.pack('<i',moved-at-4)))
    if len(writes)!=refs:
        raise LookupError(f'{len(writes)} table references, not {refs}')
    return writes


def metadata_writes(meta, segments, image=None, *, allow_address=False, validate_only=False, reserve=None):
    """Validate one patch atomically; UI may check policy/syntax without loading the ELF."""
    if meta.get('AppElf','eboot.bin')!='eboot.bin':
        raise ValueError('AppElf is not eboot.bin')
    if len(list(meta.iter('Table')))>1:
        raise ValueError('only one table can own the linked reserve')
    lines=list(meta.iter('Line'))
    if not lines:
        raise ValueError('patch has no Lines')
    sites,targets=resolve_targets(image,meta,allow_address=allow_address,segments=segments,
                                  validate_only=validate_only)
    writes=[]
    for line,offset in zip(lines,sites):
        if '{' in line.get('Value',''):
            if line.get('Type')!='bytes':
                raise ValueError('relative values require bytes')
            data=fill(line.get('Value'),offset,targets)
        else:
            data=encode(line)
        if not data:
            raise ValueError('empty patch write')
        if not validate_only and not any(start<=offset and offset+len(data)<=end for start,end in segments):
            raise ValueError(f'address {offset+EBOOT_BASE:#x} is outside the eboot')
        writes.append((offset,data))
    for table in meta.iter('Table'):
        writes += move_table(image,table,targets[table.get('Target')],reserve,segments,
                             validate_only=validate_only)
    return writes


def compile_patches(xml, names, app_version, segments, image=None, skipped=None, *, allow_address=False, reserve=None):
    """Compile selected metadata, with no absolute fallback unless its build was verified."""
    found={m.get('Name'):m for m in ET.parse(xml).getroot().iter('Metadata')
           if m.get('Name') in names and app_version in (None,m.get('AppVer'))}
    writes=[]
    table_owner=None
    for name in names:
        try:
            if name not in found:
                raise LookupError(f'patch not found for metadata version {app_version}')
            has_table=next(found[name].iter('Table'),None) is not None
            if has_table and table_owner is not None:
                raise ValueError(f'table reserve already owned by {table_owner}')
            ours=metadata_writes(found[name],segments,image,allow_address=allow_address,reserve=reserve)
            if has_table:
                table_owner=name
        except (LookupError,ValueError,TypeError,OverflowError,struct.error) as error:
            if skipped is None:
                raise ValueError(f'{name}: {error}') from error
            skipped[name]=str(error)
            continue
        writes+=ours
    return writes


PATCHES_DIR=Path(__file__).resolve().parent.parent/'patches'
# Profile selection and its XML metadata tag do not authorize absolute addresses.
# Signatures may resolve in other versions/regions; Address has its own exact-build policy.
GAMES=[({'CUSA01623','CUSA01715','CUSA01720'},PATCHES_DIR/'God_of_War_III_Remastered.xml','01.02',
        'God of War III'),
       ({'CUSA07671','CUSA05670'},PATCHES_DIR/'WipEout_Omega_Collection.xml','01.00',
        'WipEout Omega Collection')]
# Provenance verified against the original EU dump used for the B0 playtest.
ADDRESS_BUILD=('CUSA05670','01.07','3a52e910f6c450cc2f0c85ef994be5d8c951c326a362bcc4775f29e6ff7f644c')
DMEM_RETAIL_MB=5056  # runtime_memory.c POOL_SIZE default


def game_sfo(game):
    """The values of the game folder's param.sfo, or {} when it cannot be read (prepare.read_sfo)."""
    if not game:
        return {}
    try:
        from prepare import read_sfo
    except ImportError:
        return {}
    return read_sfo(Path(game) / 'sce_sys/param.sfo') or {}


def game_title_id(game):
    return game_sfo(game).get('TITLE_ID')


def game_profile(title_id, title=None):
    """(title IDs, built-in XML, metadata AppVer, title) for a known game, else None."""
    title=(title or '').replace('®','').replace('™','')
    return next((g for g in GAMES if title_id in g[0] or (title and g[3] in title)),None)


def game_profile_for(game):
    """The profile of a game folder, by title ID or by the title in its param.sfo."""
    values=game_sfo(game) if game else {}
    return game_profile(values.get('TITLE_ID'),values.get('TITLE'))


def eboot_sha256(game):
    # Permission cannot survive a same-size/mtime replacement; hash current bytes each context.
    try:
        return hashlib.sha256((Path(game)/'eboot.bin').read_bytes()).hexdigest()
    except (OSError,TypeError):
        return None


def address_patch_problem(game):
    """None only for the exact authorized title, real SFO version and current eboot.bin hash."""
    if (game_title_id(game),game_app_version(game),eboot_sha256(game))==ADDRESS_BUILD:
        return None
    return 'absolute Address requires the verified CUSA05670 / APP_VER 01.07 eboot.bin'


def game_patchable(game):
    """Whether the built-in patches are tried on this game folder (any region and version)."""
    return bool(game) and bool(game_profile_for(game))


def patch_status(out):
    """The last start's patch report (patches.py writes it): {'applied': [...], 'skipped': {...}}."""
    try:
        return json.loads((Path(out)/'patch_status.json').read_text())
    except (OSError, ValueError):
        return None


def available_patches(xml, app_version):
    """Names of the patches the XML has for this app version."""
    return {m.get('Name') for m in ET.parse(xml).getroot().iter('Metadata') if m.get('AppVer')==app_version}


# God of War III notes: the texture fix and the resolution patches both resize the video arena;
# enable one, never both. A chosen resolution patch replaces the default texture fix.
EXCLUSIVE_PREFIX='Resolution Patch'
REPLACED_BY_EXCLUSIVE={'Bug Fix - Texture Corruption Fix'}
# Fixes with nothing to choose: not offered in the launcher and applied even with its saved list,
# which predates them (the startup black screen fix sets a value the game overwrites at once).
ALWAYS_ON={'Bug Fix - Startup Black Screen', 'Bug Fix - Startup Hang', 'Bug Fix - Effects above 60 FPS',
           'Bug Fix - Zone crash above 60 FPS', 'Bug Fix - Model slots'}


def selected_patches(xml, app_version, extra, only=False):
    """The file's isEnabled patches for this version, then the names in extra (";"-separated).
    only: extra is the whole selection (the launcher's), without the file's defaults but with
    the ALWAYS_ON ones."""
    metas=[m for m in ET.parse(xml).getroot().iter('Metadata') if m.get('AppVer')==app_version]
    names=[] if only else [m.get('Name') for m in metas if m.get('isEnabled','false').lower()=='true']
    names+=[n for n in (n.strip() for n in extra.split(';')) if n and n not in names]
    names+=[m.get('Name') for m in metas if m.get('Name') in ALWAYS_ON and m.get('Name') not in names]
    exclusive=[n for n in names if n.startswith(EXCLUSIVE_PREFIX)]
    if len(exclusive)>1:
        raise ValueError(f'choose one resolution patch, not {exclusive}')
    return [n for n in names if not (exclusive and n in REPLACED_BY_EXCLUSIVE)]


def resolution_names(xmls, game=None, *, image=None, segments=None):
    """Sizes of patch entries, smallest first; a supplied game filters eligible mod metadata.
    Without game this is read-only XML introspection for built-in tooling, never authorization.
    """
    found = []
    version=game_app_version(game) if game is not None else None
    ids={game_title_id(game)} if game is not None else set()
    allow_address=address_patch_problem(game) is None if game is not None else False
    for path in xmls:
        if game is None:
            try:
                metas=list(ET.parse(path).getroot().iter('Metadata'))
            except (OSError,ET.ParseError):
                continue
        else:
            metas=eligible_metadata(path,version,ids,allow_address=allow_address,image=image,segments=segments)
        for meta in metas:
            name = meta.get('Name') or ''
            if name.startswith(EXCLUSIVE_PREFIX) and name[len(EXCLUSIVE_PREFIX):].startswith(' - '):
                size = name[len(EXCLUSIVE_PREFIX) + 3:].strip()
                if size and size not in found:
                    found.append(size)
    def pixels(size):
        width, _, height = size.partition('x')
        return (int(width) * int(height), size) if width.isdigit() and height.isdigit() else (1 << 60, size)
    return sorted(found, key=pixels)


def eligible_metadata(path, app_version, ids, *, allow_address=False, image=None, segments=None, skipped=None, reserve=None):
    """Metadata accepted before offering/merging it; runtime also resolves the real signatures."""
    accepted=[]
    try:
        root=ET.parse(path).getroot()
    except (OSError,ET.ParseError) as error:
        if skipped is not None:
            skipped[str(path)]=str(error)
        print(f'Patches: {Path(path).name}: {error}',file=sys.stderr)
        return accepted
    file_ids={e.text.strip() for e in root.iter('ID') if e.text}
    for meta in root.iter('Metadata'):
        key=f'{path}/{meta.get("Name")}'
        try:
            if not (meta.get('Name') or '').strip():
                raise ValueError('missing patch Name')
            if not app_version or meta.get('AppVer')!=app_version:
                raise ValueError(f'AppVer does not match real APP_VER {app_version or "unknown"}')
            if file_ids and not file_ids&ids:
                raise ValueError('TitleID does not match this game')
            metadata_writes(meta,segments,image,allow_address=allow_address,validate_only=image is None,reserve=reserve)
        except (LookupError,ValueError,TypeError,OverflowError,struct.error) as error:
            if skipped is not None:
                skipped[key]=str(error)
            print(f'Patches: skipped {key}: {error}',file=sys.stderr)
            continue
        accepted.append(meta)
    return accepted


def merge_patch_files(base, extras, out, *, game=None, image=None, segments=None, skipped=None, reserve=None):
    """Validate each mod metadata before replacing a built-in Name. Originals are never changed."""
    tree=ET.parse(base)
    root=tree.getroot()
    tag=next((m.get('AppVer') for m in root.iter('Metadata')),None)
    version=game_app_version(game)
    ids={game_title_id(game)}
    allow_address=address_patch_problem(game) is None
    for path in extras:
        for meta in eligible_metadata(path,version,ids,allow_address=allow_address,image=image,
                                      segments=segments,skipped=skipped,reserve=reserve):
            name=meta.get('Name')
            if next(meta.iter('Table'),None) is not None and any(m.get('Name')!=name and next(m.iter('Table'),None) is not None for m in root.iter('Metadata')):
                if skipped is not None:
                    skipped[f'{path}/{name}']='table reserve already owned by another built-in patch'
                continue
            for old in [m for m in root.iter('Metadata') if m.get('Name')==name]:
                root.remove(old)
            meta=copy.deepcopy(meta)
            # The merged selection uses the built-in XML tag; eligibility used the real SFO version.
            meta.set('AppVer',tag)
            root.append(meta)
            print(f'Patches: {name} from the mod file {Path(path).parent.parent.name}/{Path(path).name}')
    ET.indent(tree)
    Path(out).parent.mkdir(parents=True,exist_ok=True)
    tree.write(out,encoding='utf-8',xml_declaration=True)
    return Path(out)


def default_enabled(xmls, app_version, *, game=None, image=None, segments=None):
    """Enabled mod metadata of the real game version, using the same acceptance rules as merging."""
    allow_address=address_patch_problem(game) is None
    ids={game_title_id(game)}
    return [m.get('Name') for path in xmls
            for m in eligible_metadata(path,app_version,ids,allow_address=allow_address,image=image,segments=segments)
            if m.get('isEnabled','false').lower()=='true']


def patch_requirements(xml, names, app_version):
    """(direct memory MiB, VBlank Hz) the selected patches' notes ask for, None when not needed.
    shadPS4 notes say "Requires setting DMEM to N MB": N is on top of the retail pool."""
    dmem=vblank=None
    for meta in ET.parse(xml).getroot().iter('Metadata'):
        if meta.get('Name') not in names or meta.get('AppVer')!=app_version: continue
        note=meta.get('Note','')
        if m:=re.search(r'DMEM to (\d+) MB',note): dmem=max(dmem or 0,DMEM_RETAIL_MB+int(m.group(1)))
        if m:=re.search(r'VBlank to (\d+) FPS',note): vblank=max(vblank or 0,int(m.group(1)))
    return dmem,vblank


def external_patches(directory, app_version, exclude, ids, *, game=None, skipped=None):
    """External metadata for the real APP_VER and actual TitleID, under the shared safety policy."""
    found=[]
    directory=Path(directory)
    allow_address=address_patch_problem(game) is None
    for path in sorted(directory.glob('*.xml')) if directory.is_dir() else []:
        if path.resolve()==Path(exclude).resolve() or path.resolve() in {g[1].resolve() for g in GAMES}:
            continue
        for meta in eligible_metadata(path,app_version,ids,allow_address=allow_address,skipped=skipped):
            found.append((f'{path.name}/{meta.get("Name")}',path,meta))
    return found


def external_selection(found, config):
    """Selected external patches: patches.json {"enabled": [...], "disabled": [...]} overrides
    each file's isEnabled."""
    settings={}
    if config and Path(config).is_file():
        settings=json.loads(Path(config).read_text())
    enabled,disabled=set(settings.get('enabled',[])),set(settings.get('disabled',[]))
    return [(key,path,meta) for key,path,meta in found
            if key in enabled or (key not in disabled and meta.get('isEnabled','false').lower()=='true')]


def compile_external(selected, segments, image=None, *, allow_address=False, skipped=None, reserve=None, table_owner=None):
    """External patches use the same atomic resolver and absolute-address policy as built-ins."""
    writes=[]
    for key,_,meta in selected:
        try:
            has_table=next(meta.iter('Table'),None) is not None
            if has_table and table_owner is not None:
                raise ValueError(f'table reserve already owned by {table_owner}')
            ours=metadata_writes(meta,segments,image,allow_address=allow_address,reserve=reserve)
            if has_table:
                table_owner=key
        except (LookupError,ValueError,TypeError,OverflowError,struct.error) as error:
            if skipped is not None:
                skipped[key]=str(error)
            print(f'Patches: skipped {key}: {error}',file=sys.stderr)
            continue
        writes+=ours
        print(f'Patches: external {key} ({len(ours)} writes)')
    return writes


def write_patches(out, writes):
    # G3PATCH2: the patch base, so the loader can rebase pointers the patches write into
    # relocated slots.
    blob=struct.pack('<8sQQ',b'G3PATCH2',EBOOT_BASE,len(writes))
    for offset,data in writes: blob+=struct.pack('<QQ',offset,len(data))+data
    (out/'patches.bin').write_bytes(blob)


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--patches-dir',type=Path,help='third-party patch XML files (shadPS4 format)')
    p.add_argument('--patches-config',type=Path,help='patches.json: enabled/disabled external patches')
    p.add_argument('--mod-patches',type=Path,action='append',default=[],
                   help='patch file of an enabled mod (repeat; in load order)')
    p.add_argument('--extra',default='',help='additional patch names, separated by ";"')
    p.add_argument('--out',type=Path,default=Path(__file__).resolve().parent.parent/'out')
    p.add_argument('--game-dir',type=Path,default=Path(os.environ.get('GOW3_GAME_DIR','../CUSA01623')))
    a=p.parse_args()
    title=game_title_id(a.game_dir)
    profile=game_profile_for(a.game_dir)
    if not profile:
        write_patches(a.out,[])
        print(f'Patches: no patch profile for {title}: none applied')
        return
    _ids,xml,tag,_name=profile
    sha=eboot_sha256(a.game_dir)
    version=game_app_version(a.game_dir)
    allow_address=(title,version,sha)==ADDRESS_BUILD
    print(f'Patches: {title} APP_VER {version}, XML tag {tag}, eboot.bin sha256 {sha}')
    elf=(a.out/'eboot.elf').read_bytes()
    segments=eboot_segments(elf)
    image=eboot_image(elf)
    link=json.loads((a.out/'link.json').read_text()) if (a.out/'link.json').is_file() else {}
    record=link.get('reserve')
    reserve=(int(record['base'],0),record['size']) if record else None
    mod_skipped={}
    if a.mod_patches:
        xml=merge_patch_files(xml,a.mod_patches,a.out/'patches-merged.xml',game=a.game_dir,
                              image=image,segments=segments,skipped=mod_skipped,reserve=reserve)
    names=selected_patches(xml,tag,a.extra,os.environ.get('GOW3_PATCHES_ONLY')=='1')
    skipped={}
    writes=compile_patches(xml,names,tag,segments,image,skipped,allow_address=allow_address,reserve=reserve)
    for name,reason in skipped.items():
        print(f'Patches: {name} skipped ({reason} in this eboot.bin)')
    table_owner=next((m.get('Name') for m in ET.parse(xml).getroot().iter('Metadata')
                      if m.get('Name') in names and m.get('Name') not in skipped and next(m.iter('Table'),None) is not None),None)
    external_skipped={}
    if a.patches_dir:
        found=external_patches(a.patches_dir,version,xml,{title},game=a.game_dir,skipped=external_skipped)
        writes+=compile_external(external_selection(found,a.patches_config),segments,image,
                                 allow_address=allow_address,skipped=external_skipped,reserve=reserve,table_owner=table_owner)
    write_patches(a.out,writes)
    applied=[n for n in names if n not in skipped]
    if skipped and not applied:
        # Every signature misses: in the reports (#39, #54) an eboot.bin without its update.
        print('Patches: none of the patches fit this eboot.bin: is the game update installed? '
              '(launcher: Game > Folders > Update folder)')
    (a.out/'patch_status.json').write_text(json.dumps({'sha256':sha,'app_version':version,'xml_tag':tag,'applied':applied,'skipped':skipped,
                                                                   'mod_skipped':mod_skipped,'external_skipped':external_skipped}))
    print(f'Patches: {title}; {len(writes)} writes from {applied or "none"}')

if __name__=='__main__':
    main()
