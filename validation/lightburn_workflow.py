"""Check file interchange in the same editor the operator uses."""
from playwright.sync_api import expect


def run(host):
    with host.context.browser.new_context() as context:
        page=context.new_page()
        page.on('dialog',lambda d:d.accept())
        page.goto(host.url)
        page.wait_for_function('!!window.studioSession')
        loc=page.locator
        tab=lambda name:page.get_by_role('tab',name=name,exact=True).click()
        tab('Projects')
        loc('#import-example').click()
        expect(loc('#import-summary')).to_contain_text('1 objects')
        assert page.evaluate('CNCProjects.document().objects.length') == 0
        loc('#import-commit').click()
        layer=page.evaluate('projectExtras.designLayers.at(-1)')
        assert layer['settings']=={'operation':'line','feed':480,'power':350}
        assert not layer['output']
        tab('Layers')
        loc('#designLayerList>div').last.get_by_label('Output').check()
        tab('CAM')
        loc('#preview').click()
        expect(loc('#message')).to_contain_text('Preview ready')
        assert page.evaluate('preview.lines.some(s=>s.includes("S350"))')
        tab('Projects')
        loc('#projectName').fill('Practice badge')
        loc('#projectSaveLibrary').click()
        row=loc('#projectList p').filter(has_text='Practice badge')
        expect(row).to_have_count(1)
        loc('#projectNew').click()
        row.get_by_role('button',name='Open',exact=True).click()
        expect(loc('#projectName')).to_have_value('Practice badge')
        assert page.evaluate('projectExtras.designLayers.at(-1).settings.feed')==480
        print('PASS LightBurn sample, layer conversion, emitted power and saved project')

        samples=[
            ('''<LightBurnProject><Shape Type="Group"><XForm>2 0 0 2 10 10</XForm><Children>
            <Shape Type="Path" VertID="1" PrimID="2"><VertList>V0 0c0x0c0y3V4 0c1x4c1y3</VertList>
            <PrimList>B0 1</PrimList></Shape><Shape Type="Path" VertID="1" PrimID="2"><XForm>1 0 0 1 0 1</XForm></Shape>
            </Children></Shape></LightBurnProject>''','packed.lbrn2'),
            ('''<LightBurnProject><Shape Type="Path"><V vx="1" vy="2"/><V vx="4" vy="6"/>
            <P T="L" p0="0" p1="1"/></Shape></LightBurnProject>''','legacy.lbrn'),
            ('''<LightBurnLibrary><Material name="Wood"><Entry Thickness="3" Desc="Score">
            <CutSetting type="Cut"><speed Value="10"/><maxPower Value="20"/></CutSetting></Entry></Material></LightBurnLibrary>''','materials.clb')]
        packed,legacy,clb=[page.evaluate('([s,n])=>CNCLightBurn.read(n,s)',sample) for sample in samples]
        assert len(packed['objects'])==2
        curve=packed['objects'][0]['paths'][0]
        assert curve[0]==[10,10] and curve[-1]==[18,10] and max(y for x,y in curve)>14
        assert legacy['objects'][0]['paths']==[[[1,2],[4,6]]]
        assert clb['presets'][0]['settings']['feed']==600
        for invalid in ['<broken/>','<!DOCTYPE x><LightBurnProject/>',
                        '<LightBurnProject><Shape Type="Path"><VertList>V1 2</VertList><PrimList>L0 99</PrimList></Shape></LightBurnProject>']:
            assert page.evaluate('(s)=>{try{CNCLightBurn.read("invalid.lbrn2",s);return false}catch{return true}}',invalid)
        print('PASS legacy vertices, transformed shared cubic, material preset and invalid documents')
