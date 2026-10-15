"""Synthetic-only schema, conversion, archive resolution and merge tests."""
import copy
import pathlib
import struct
import tempfile
import unittest
import zlib
from unittest.mock import patch
import shaderprep as prep
import shaderprep_format as fmt


def compact(attributes):
    return b'WWFS'+struct.pack('>III',len(attributes),0,0)+b''.join(struct.pack('>4I',*a) for a in attributes)


def metal():
    return {'backend':'metal','shader_hashes':[1,2,3],'formats':[0]*9,'blend':[0]*8,
            'color_control':0,'target_mask':0,'strides':[0]*16}


def vulkan():
    state={k:[0]*n for k,n in fmt.VK_GROUPS.items()}
    state['viewport']=[0,1,1];state['raster'][10]=0x3f800000;state['multisample'][1]=1
    return {'backend':'vulkan','shader_hashes':[1,2,3],'key_words':[0]*46,
            'dynamic_uniforms':0,'descriptors':[[],[]],'rendering':[0,0,0],
            'formats':[],'bindings':[],'attributes':[],'state':state,'attachments':[],'dynamics':[0,1,4,8]}


class CodefreeTemplate(unittest.TestCase):
    def test_linked_pixel_roundtrip_and_archive_resolution(self):
        pixel=b'pixel synthetic program';vertex=b'vertex synthetic program'
        fetch=compact([(0,0,0,0)])
        regs=[(0xa214,7),(0xc000,0xdeadbeef)]
        raw=prep.linked_shader_record(pixel,fetch,vertex,regs)
        self.assertEqual(prep.parse_linked_shader_record(raw),(pixel,fetch,vertex,regs))
        for invalid in (raw[:19],raw[:-1],raw+b'x'):
            with self.assertRaises(ValueError):prep.parse_linked_shader_record(invalid)
        with tempfile.TemporaryDirectory() as d:
            d=pathlib.Path(d);rec=d/'record';template=d/'template';out=d/'out'
            with rec.open('wb') as f:prep.write_record(f,6,raw)
            prep.cmd_template(rec,template)
            doc=fmt.decode(template.read_bytes());state=doc['states'][0]
            self.assertIsNone(state['fetch'])
            self.assertEqual(state['linked_vertex']['hash'],prep.prog_hash(vertex))
            self.assertNotIn(pixel,template.read_bytes());self.assertNotIn(vertex,template.read_bytes())
            self.assertEqual(state['registers'],[[0xa214,7]])
            games={(prep.PS,pixel):([0]*64,'synthetic'),(prep.VS,vertex):([0]*64,'synthetic')}
            with patch.object(prep,'game_shaders',return_value=games):prep.cmd_build(d,out,template,0)
            records=list(prep.read_records(out));self.assertEqual([t for t,_ in records],[6])
            ps,fs,vs,r=prep.parse_linked_shader_record(records[0][1])
            self.assertEqual((ps,vs),(pixel,vertex))
            self.assertEqual(fmt.parse_fetch(fs,r,prep.prog_hash),state['linked_vertex']['fetch'])
            with patch.object(prep,'game_shaders',return_value={(prep.PS,pixel):([0]*64,'synthetic')}):
                prep.cmd_build(d,out,template,0)
            self.assertEqual(out.read_bytes(),b'') # No silently unlinked approximation.
            bad=copy.deepcopy(doc);bad['states'][0]['vertex']=1
            with self.assertRaises(ValueError):fmt.encode(bad)
            bad=copy.deepcopy(doc);bad['states'][0]['linked_vertex']['fetch']=None
            with self.assertRaises(ValueError):fmt.encode(bad)

    def test_native_fetch_hash_vectors_are_not_archive_lookup_hashes(self):
        # Native hash_bytes: seed, little-endian 8-byte mixer, bytewise tail,
        # final xor29. These fixed vectors exercise empty/full-word/tail cases.
        vectors=((b'',0x9e3779bd8ef1b1de),(b'a',0x22c04a325d932886),
                 (b'abcdefgh',0x82ddd764292bbe85),(b'abcdefghi',0x23fae58d441e3dd5))
        for data,expected in vectors:
            self.assertEqual(fmt.metal_fetch_hash(data),expected)
            self.assertNotEqual(expected,prep.prog_hash(data))

    def test_compact_semantics_group_order_and_regeneration(self):
        # Compact builder sorts buffers and discards invalid buffers. Endian
        # DEFAULT and divisor0 are resolved exactly as the HLE builder does.
        source=compact([(1|(2<<8)|(1<<16)|(3<<24),4,7,0x00010203),
                        (2|(0<<8)|(3<<24),0,0,0x04050607),
                        (3|(20<<8),0,0,0)])
        desc=fmt.parse_fetch(source,[],prep.prog_hash)
        self.assertEqual(desc['compact'],1)
        self.assertEqual(desc['metal_hash'],fmt.metal_fetch_hash(source))
        self.assertEqual(desc['hash'],desc['metal_hash'])
        self.assertNotEqual(desc['hash'],prep.prog_hash(source))
        self.assertEqual([a['buffer'] for a in desc['attributes']],[0,2])
        self.assertEqual(desc['attributes'][0]['endian'],0)
        self.assertEqual(desc['attributes'][1]['endian'],1)
        self.assertEqual(desc['attributes'][1]['divisor'],1)
        self.assertEqual(fmt.parse_fetch(fmt.fetch_payload(desc),[],prep.prog_hash),desc)
        bad=copy.deepcopy(desc);bad['compact']=2
        with self.assertRaises(ValueError):fmt.fetch_payload(bad)
        self.assertNotIn(source,fmt.fetch_payload(desc))

    def test_legacy_semantics_divisors_and_fail_closed(self):
        # Synthetic semantic instruction uses register-selected instance divisor.
        words=[1|(1<<5)|(160<<8)|(1<<24),5|(1<<9)|(2<<12)|(3<<15)|(4<<18)|(1<<22),8|(2<<16),0]
        raw=struct.pack('<4I',*words)
        desc=fmt.parse_fetch(raw,[(0xa2a8,7)],prep.prog_hash)
        self.assertEqual(desc['compact'],0)
        a=desc['attributes'][0]
        self.assertEqual((a['semantic'],a['divisor'],a['endian'],a['offset']),(5,7,2,8))
        with self.assertRaises(ValueError):fmt.parse_fetch(raw,[],prep.prog_hash)
        for index,bit in ((0,1<<23),(1,1<<21),(2,1<<18),(3,1)):
            bad=words.copy();bad[index]|=bit
            with self.assertRaises(ValueError):fmt.parse_fetch(struct.pack('<4I',*bad),[(0xa2a8,7)],prep.prog_hash)
        with self.assertRaises(ValueError):fmt.parse_fetch(b'GPU code',[],prep.prog_hash)
        # Native legacy parser ignores MEM/clause headers, but not other opcodes.
        header=struct.pack('<4I',2,0,0,0)
        self.assertEqual(fmt.parse_fetch(header+raw,[(0xa2a8,7)],prep.prog_hash)['attributes'],[a])
        with self.assertRaises(ValueError):fmt.parse_fetch(struct.pack('<4I',3,0,0,0)+raw,[(0xa2a8,7)],prep.prog_hash)

    def test_legacy_cf_bounds_and_termination(self):
        ins=struct.pack('<4I',1|(160<<8),1<<22,0,0)
        cf=struct.pack('<4I',2,3<<23,0,(20<<23)|(1<<31))
        self.assertEqual(len(fmt.parse_fetch(cf+ins,[],prep.prog_hash)['attributes']),1)
        bad=bytearray(cf+ins);struct.pack_into('<I',bad,0,100)
        with self.assertRaises(ValueError):fmt.parse_fetch(bytes(bad),[],prep.prog_hash)

    def test_strict_template_no_opaque_fields(self):
        doc={'version':3,'states':[{'hash':1,'vertex':1,'fetch':None,'registers':[[0xa214,2]]}], 'pipelines':[metal(),vulkan()]}
        self.assertEqual(fmt.decode(fmt.encode(doc)),doc)
        for field in ('microcode','fetch_bytes','spirv','source','native_cache'):
            bad=copy.deepcopy(doc);bad['states'][0][field]=[1,2,3]
            with self.assertRaises(ValueError):fmt.encode(bad)
        for a in (0xa210,0xa216,0xa225,0):
            bad=copy.deepcopy(doc);bad['states'][0]['registers']=[[a,1]]
            with self.assertRaises(ValueError):fmt.encode(bad)
        for data in (fmt.encode(doc)+b'x',fmt.encode(doc)[:-1],b'WWHT0002'+zlib.compress(b'x')):
            with self.assertRaises(ValueError):fmt.decode(data)
        with self.assertRaises(ValueError):fmt.decode(fmt.MAGIC+zlib.compress(b'{"version":3,"version":3,"states":[],"pipelines":[]}'))

    def test_typed_pipeline_roundtrip_and_code_exclusion(self):
        p=metal();p['strides'][0]=48;self.assertEqual(fmt.metal_pipeline(fmt.metal_payload(p)),p)
        p=vulkan();raw=fmt.vulkan_payload(p)
        self.assertTrue(raw.startswith(b'WWVKST01'))
        self.assertEqual(fmt.vulkan_recipe(raw[8:],codefree=True),p)
        w=fmt.Words(raw[8:]);self.assertEqual(len(w.vec(208)),208);self.assertEqual(w.scalar(),0)
        for _ in range(2):self.assertEqual(w.vec(262144),[]);self.assertEqual(w.scalar(),0)
        # Local Vulkan recording contains code; conversion strips both stage
        # vectors while retaining every typed descriptor/fixed-state field.
        words=list(struct.unpack('<%dI'%((len(raw)-8)//4),raw[8:]))
        at=210
        code=[0x07230203,0x00010300,0,1,0,0x00010000]
        for _ in range(2):
            self.assertEqual(words[at],0)
            words[at:at+1]=[len(code),*code]
            at+=1+len(code)
            count=words[at];at+=1+4*count
        local=struct.pack('<%dI'%len(words),*words)
        self.assertEqual(fmt.vulkan_recipe(local),p)
        dbbody=struct.pack('<I',len(local))+local
        db=b'WWVKPR01'+b'\0'*48+struct.pack('<II',1,zlib.crc32(dbbody))+dbbody
        self.assertEqual(fmt.vulkan_database(db),[p])
        self.assertNotIn(struct.pack('<6I',*code),fmt.vulkan_payload(p))
        bad=copy.deepcopy(p);bad['spirv']=[0x07230203]
        with self.assertRaises(ValueError):fmt.vulkan_payload(bad)
        bad=copy.deepcopy(p);bad['descriptors'][0]=[[1,1,1,16]]
        with self.assertRaises(ValueError):fmt.vulkan_payload(bad)

    def test_merge_build_archive_resolution_local_payload(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=pathlib.Path(tmp);record=root/'record';template=root/'template';merged=root/'merged';out=root/'headstart'
            code=b'synthetic program';fs=compact([(160,0,0,0)])
            with record.open('wb') as f:
                prep.write_record(f,1,prep.shader_record(1,code,fs,[(0xa216,123),(0xa21a,1)]))
                prep.write_record(f,2,fmt.metal_payload(metal()))
            prep.cmd_template(record,template);prep.cmd_template(record,merged,template)
            self.assertEqual(prep.read_template(template),prep.read_template(merged))
            self.assertNotIn(code,fmt.encode(fmt.decode(template.read_bytes())))
            with patch.object(prep,'game_shaders',return_value={(prep.VS,code):([0]*64,'fixture')}):
                prep.cmd_build('',out,merged,0)
            records=list(prep.read_records(out));self.assertEqual([t for t,_ in records],[1,2])
            _,resolved,fetch,_=prep.parse_shader_record(records[0][1])
            self.assertEqual(resolved,code);self.assertTrue(fetch.startswith(fmt.FETCH_MAGIC))
            with patch.object(prep,'game_shaders',return_value={}):prep.cmd_build('',out,merged,0,False)
            self.assertEqual(list(prep.read_records(out)),[])

    def test_uniform_data_stripped_not_accepted_by_schema(self):
        state=prep._codefree_state(1,0,compact([(0,0,0,0)]),
            [(0xa214,5),(0xc000,123),(0xc01f,456),(0xc400,789),(0xc48f,10),(0xdfff,11)])
        self.assertEqual(state['registers'],[[0xa214,5]])
        self.assertIsNone(state['fetch'])
        fmt.validate_document({'version':3,'states':[state],'pipelines':[]})
        for address in (0xc000,0xc400,0xdfff):
            bad=copy.deepcopy(state);bad['registers'].append([address,123])
            with self.assertRaises(ValueError):fmt.encode({'version':3,'states':[bad],'pipelines':[]})
        # An unknown register outside the allowed structural banks stays fatal.
        bad=prep._codefree_state(1,0,b'',[(0x1000,123)])
        with self.assertRaises(ValueError):fmt.encode({'version':3,'states':[bad],'pipelines':[]})

    def test_corrupt_or_truncated_record(self):
        with tempfile.TemporaryDirectory() as tmp:
            path=pathlib.Path(tmp)/'record'
            with path.open('wb') as f:prep.write_record(f,4,b'pipeline')
            data=path.read_bytes()
            for bad in (data[:-1],data[:11],data+b'x'):
                path.write_bytes(bad)
                with self.assertRaises(ValueError):list(prep.read_records(path))


def sharcfb(programs):
    """synthetic BAHS archive holding (type, microcode) vertex/pixel binaries"""
    entries=b''
    for typ,code in programs:
        head=0xDC if typ==prep.VS else 0xAC
        data=bytearray(head)+code
        struct.pack_into('<II',data,0xD0 if typ==prep.VS else 0xA4,len(code),head)
        entries+=struct.pack('<4I',16+len(data),typ,0,len(data))+bytes(data)
    name=b'synthetic\0'
    header=b'BAHS'+bytes(16)+struct.pack('<I',len(name))+name
    return header+struct.pack('<II',8+len(entries),len(programs))+entries


def yaz0_literal(raw):
    """valid Yaz0 stream made of literal groups only"""
    out=bytearray(b'Yaz0'+struct.pack('>I',len(raw))+bytes(8))
    for i in range(0,len(raw),8):
        out.append(0xFF);out+=raw[i:i+8]
    return bytes(out)


class ParallelBuild(unittest.TestCase):
    def game(self, root):
        programs=[(prep.VS if i%2 else prep.PS,('synthetic program %03d'%i).encode().ljust(24+i%5,b'.')) for i in range(40)]
        game=root/'content';game.mkdir()
        for n in range(6):
            chunk=programs[n*5:n*5+12]  # overlapping: the same program in several archives
            data=sharcfb(chunk)
            (game/('a%d.szs'%n)).write_bytes(yaz0_literal(data) if n%2 else data)
        (game/'ignored.txt').write_text('not an archive')
        return game,programs

    def test_parallel_scan_and_build_are_identical_with_monotonic_progress(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=pathlib.Path(tmp);game,programs=self.game(root)
            serial=prep.game_shaders(str(game))
            seen=[]
            parallel=prep.game_shaders(str(game),lambda *a:seen.append(a),jobs=2)
            self.assertEqual(list(serial.items()),list(parallel.items()))
            self.assertEqual(len(serial),len({c for _,c in programs[:37]}))
            self.assertEqual(seen[0][0],0);self.assertEqual(seen[-1][0],seen[-1][1])
            self.assertEqual([s[2] for s in seen],list(range(len(seen))))
            record=root/'record';template=root/'template'
            with record.open('wb') as f:
                for typ,code in programs[:20]:
                    prep.write_record(f,1,prep.shader_record(1 if typ==prep.VS else 0,code,
                        compact([(160,0,0,0)]) if typ==prep.VS else b'',[(0xa21a if typ==prep.VS else 0xa214,7)]))
                prep.write_record(f,2,fmt.metal_payload(metal()))
            prep.cmd_template(record,template)
            outputs=[]
            for jobs in (1,3):
                out=root/('headstart%d'%jobs);steps=[]
                prep.cmd_build(str(game),str(out),str(template),0,progress=lambda d,t,detail:steps.append((d,t,detail)),jobs=jobs)
                outputs.append(out.read_bytes())
                done=[d for d,_,_ in steps]
                self.assertEqual(done,sorted(done));self.assertEqual(steps[-1][:2],(1000,1000))
                self.assertTrue(all(t==1000 for _,t,_ in steps))
                self.assertIn('game archives',steps[1][2])
            self.assertEqual(outputs[0],outputs[1])
            self.assertEqual([t for t,_ in prep.read_records(root/'headstart1')],[1]*20+[2])

    def test_thread_scan_matches_serial(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=pathlib.Path(tmp);game,_=self.game(root)
            serial=prep.game_shaders(str(game))
            seen=[]
            threaded=prep.game_shaders(str(game),lambda *a:seen.append(a),jobs=2,threads=True)
            self.assertEqual(list(serial.items()),list(threaded.items()))
            self.assertEqual(seen[-1][2],seen[-1][3])

    def test_fast_hash_matches_reference(self):
        def reference(code):
            h=0xCBF29CE484222325
            for i in range(0,len(code),8):
                h=((h^int.from_bytes(code[i:i+8].ljust(8,b'\0'),'little'))*0x100000001B3)&0xFFFFFFFFFFFFFFFF
            return h^len(code)
        for n in range(0,40):
            code=bytes((i*37+n)&255 for i in range(n))
            self.assertEqual(prep.prog_hash(code),reference(code))

    def test_cli_progress_lines(self):
        import subprocess,sys
        with tempfile.TemporaryDirectory() as tmp:
            root=pathlib.Path(tmp);game,programs=self.game(root)
            record=root/'record';template=root/'template'
            with record.open('wb') as f:prep.write_record(f,2,fmt.metal_payload(metal()))
            prep.cmd_template(record,template)
            out=subprocess.run([sys.executable,str(pathlib.Path(prep.__file__)),'build',str(root/'hs'),'--game',str(game),
                '--template',str(template),'--max','0','--progress','--jobs','2'],capture_output=True,text=True,check=True).stdout
            lines=[l for l in out.splitlines() if l.startswith('WWHD-PROGRESS ')]
            self.assertTrue(lines);self.assertTrue(lines[-1].startswith('WWHD-PROGRESS 1000 1000 '))


class Yaz0Loader(unittest.TestCase):
    def setUp(self):
        temporary=tempfile.TemporaryDirectory();self.addCleanup(temporary.cleanup)
        self.root=pathlib.Path(temporary.name)
        self.explicit=self.root/'explicit/libyaz0.so';self.native=self.root/'native/libyaz0.so'
        self.packaged=self.root/('package/tools/lib/libyaz0'+prep.YAZ0_SUFFIX)
        for path in (self.explicit,self.native,self.packaged):
            path.parent.mkdir(parents=True);path.write_bytes(b'synthetic library')

    def candidates(self, environ, android_dir=''):
        with patch.object(prep,'PACKAGED_YAZ0',str(self.packaged)):
            return prep.yaz0_candidates(environ, android_dir)

    def test_order_explicit_android_packaged(self):
        self.assertEqual(self.candidates({'WWHD_YAZ0_LIB':str(self.explicit)},str(self.native.parent)),
                         [str(self.explicit),str(self.native),str(self.packaged)])
        self.assertEqual(self.candidates({}),[str(self.packaged)])
        # missing files are skipped, duplicates collapse
        self.assertEqual(self.candidates({'WWHD_YAZ0_LIB':str(self.root/'missing.so')},str(self.root/'none')),
                         [str(self.packaged)])
        self.assertEqual(self.candidates({'WWHD_YAZ0_LIB':str(self.native)},str(self.native.parent)),
                         [str(self.native),str(self.packaged)])

    def test_android_library_dir_from_process_maps(self):
        maps=self.root/'maps'
        maps.write_text('7000-8000 r-xp 00000000 fd:00 1 /system/lib64/libc.so\n'
                        '7100-7200 r-xp 00000000 fd:00 2 /data/app/~~x==/org.wwhdrecomp.wwhd-y==/lib/arm64/libpython3.14.so\n'
                        '7300-7400 rw-p 00000000 00:00 0\n')
        with patch.object(prep.sys,'platform','android'):
            self.assertEqual(prep.android_library_dir(str(maps)),'/data/app/~~x==/org.wwhdrecomp.wwhd-y==/lib/arm64')
            self.assertIsNone(prep.android_library_dir(str(self.root/'no-maps')))
        with patch.object(prep.sys,'platform','linux'):
            if not hasattr(prep.sys,'getandroidapilevel'):
                self.assertIsNone(prep.android_library_dir(str(maps)))

    def test_prebuilt_preferred_then_next_then_source_build(self):
        loaded=[]
        def cdll(path):
            loaded.append(path)
            if path==str(self.explicit): raise OSError('wrong architecture')
            return 'library:'+path
        with patch.object(prep,'PACKAGED_YAZ0',str(self.packaged)), patch.object(prep.ctypes,'CDLL',side_effect=cdll), \
                patch.dict(prep.os.environ,{'WWHD_YAZ0_LIB':str(self.explicit)}), \
                patch.object(prep,'android_library_dir',return_value=str(self.native.parent)), \
                patch('sys.stderr'):
            self.assertEqual(prep._load_yaz0(),'library:'+str(self.native))
        self.assertEqual(loaded,[str(self.explicit),str(self.native)])
        # a source checkout with only the packaged copy builds its own decoder
        with patch.object(prep,'PACKAGED_YAZ0',str(self.packaged)), \
                patch.dict(prep.os.environ,{},clear=False), \
                patch.object(prep,'android_library_dir',return_value=None), \
                patch.object(prep,'yaz0_candidates',return_value=[str(self.packaged)]), \
                patch.object(prep.os.path,'getmtime',return_value=0), \
                patch.object(prep.ctypes,'CDLL',side_effect=lambda path:'library:'+path):
            self.assertTrue(prep._load_yaz0().endswith('build/libyaz0'+prep.YAZ0_SUFFIX))

if __name__=='__main__':unittest.main()
