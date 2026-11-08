package com.tyranor.runtime.systemnnn;

import java.io.File;
import java.util.*;

public final class GameStoryControlsTest {
    private static int checks;
    private static void check(boolean value,String message){checks++;if(!value)throw new AssertionError(message);}
    public static void main(String[] roots)throws Exception{
        for(String name:roots){
            File root=new File(name);Map<String,GameUiAssets.Config> configurations=new LinkedHashMap<>();
            for(String config:GameStoryControlsModel.CONFIG_NAMES)configurations.put(config,GameUiAssets.readConfig(root,config));
            GameStoryControlsModel model=new GameStoryControlsModel(GameUiAssets.readConfig(root,"game"),GameUiAssets.readConfig(root,"printmessage"),configurations);
            boolean mng=root.getName().equals("鏖殺ノ乙女");
            check(model.original&&model.buttons.size()==3,"Original buttons missing: "+name);
            check(!model.drawDuringTransitions&&model.largePrintOptions,"Source option visibility: "+name);
            Set<GameStoryControlsModel.Action> actions=new HashSet<>();
            for(GameStoryControlsModel.Button b:model.buttons){
                actions.add(b.action);
                check(b.x==1098&&b.width==182&&b.height==48,"Original coordinate/size");
                check(b.art.states[0].x==0&&b.art.states[0].y==0&&b.art.states[2].y==336&&b.art.states[1].y==48,"Original sprite states");
                check(b.art.states[1].frames==6&&b.art.states[1].type==6,"Original hover animation");
                check(b.checkX==0&&b.checkY==384&&b.checkDeltaY==7&&b.checkWidth==182&&b.checkHeight==48,"Original alpha hit mask");
                check(b.clickTime==10,"Source click wait");
                if(b.action==GameStoryControlsModel.Action.MENU){check(b.y==(mng?667:566)&&b.sound==7&&b.art.image.equals("ta_q_sys01"),"Menu position/sound differs by game");}
                else if(b.action==GameStoryControlsModel.Action.QUICK_SAVE){check(b.y==(mng?566:667)&&b.sound==3&&b.art.image.equals("ta_q_save01"),"QuickSave source contract");}
                else if(b.action==GameStoryControlsModel.Action.QUICK_LOAD){check(b.y==616&&b.sound==3&&b.art.image.equals("ta_q_load01"),"QuickLoad source contract");}
                else throw new AssertionError("Invented control on original scene");
            }
            check(actions.size()==3&&!actions.contains(GameStoryControlsModel.Action.AUTO)&&!actions.contains(GameStoryControlsModel.Action.SKIP),"Unexpected toolbar instead of source controls");
        }
        System.out.println("PASS "+checks+" original story control checks");
    }
}
