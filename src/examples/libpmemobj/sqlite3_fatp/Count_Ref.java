import java.io.BufferedReader;
import java.io.FileReader;
import java.io.IOException;
import java.nio.file.Files;
import java.util.HashMap;

public class Count_Ref {

	public static void main(String[] args) {
		String fileName = "memory_references.txt";
		HashMap<Long, Boolean> map = new HashMap<Long, Boolean>();
		FileReader reader = null;
		BufferedReader bf = null;
		String address = "";
		Long address_value = 0L;
		Long total_references = 0L;
		try{
			reader = new FileReader(fileName);
			bf = new BufferedReader(reader);
			int i = 0;
			while((address = bf.readLine()) != null){
				address_value = Long.parseLong(address, 16);//Integer.parseInt(address, 16);
				//System.out.println("address "+address_value);
				if(!map.containsKey(address_value)){
					map.put(address_value, true);
				}
				total_references++;
			}		
		}catch(IOException e){
			System.out.println("exception "+e.getMessage());
		}
		System.out.println("number of distinct memory references "+map.size());
		System.out.println("total number of references "+total_references);
	}
}
